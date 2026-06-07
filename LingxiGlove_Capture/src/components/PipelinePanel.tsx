/**
 * PipelinePanel — Day 3 数据流水线面板
 *
 * 折叠了三个子卡片：
 *   1. Sessions：列出 out_root 下所有 session_<ts>/raw.csv（行数 + 已打标行数）
 *   2. Settings：build_dataset.py 路径 / python 解释器 / dataset 输出根 / EI api key
 *   3. Run：一键 build_dataset → upload，实时显示进度日志
 *
 * 设置除 EI api key 外都存 localStorage（key=lingxi-capture/pipeline-settings）；
 * EI key 存 macOS Keychain（通过 set_ei_key/has_ei_key/delete_ei_key）。
 */
import { useCallback, useEffect, useMemo, useRef, useState } from "react";
import { listen } from "@tauri-apps/api/event";
import { open as openDialog, ask, message } from "@tauri-apps/plugin-dialog";
import {
  deleteEiKey,
  deleteSession,
  getDefaultScriptPath,
  getOutRoot,
  hasEiKey,
  listSessions,
  runBuildDataset,
  runPipeline,
  setEiKey,
  setOutRoot as setOutRootApi,
  uploadToEi,
} from "../api";
import type {
  BuildDatasetArgs,
  PipelineProgress,
  SessionEntry,
} from "../types";
import { LABEL_NAMES } from "../types";
import { useCaptureStore } from "../store";

/**
 * 将 SessionEntry.label_counts 翻译为可读字符串。
 *
 * 优先级：用户自定义 labelNames[i] > LABEL_NAMES[i] > `label_<i>`。
 * 返回 Array<{ key, name, count, isUnlabeled }> 供渲染。
 */
function formatLabelCounts(
  counts: ReadonlyArray<readonly [number, number]>,
  labelNames: readonly string[],
): Array<{ key: number; name: string; count: number; isUnlabeled: boolean }> {
  return counts.map(([k, n]) => {
    let name: string;
    if (k === -1) {
      name = "unlabeled";
    } else if (k >= 0 && k < labelNames.length && labelNames[k]?.trim()) {
      name = labelNames[k];
    } else if (k in LABEL_NAMES) {
      name = LABEL_NAMES[k as keyof typeof LABEL_NAMES];
    } else {
      name = `label_${k}`;
    }
    return { key: k, name, count: n, isUnlabeled: k === -1 };
  });
}

const LS_KEY = "lingxi-capture/pipeline-settings";

/**
 * 历史版本写死过、但在多机器上都不一定存在的脚本路径。
 * 初始化时如果 localStorage 里仍是这些路径，自动迁移为打包内默认路径。
 */
const LEGACY_SCRIPT_PATHS = [
  "/Users/kun.li/Code/Lingxi/LingxiGlove/tools/build_dataset.py",
];

/**
 * 「额外参数」弹层参数表。
 * 与 build_dataset.py 的 argparse 保持同步；修改脚本默认值后记得同步这里。
 *
 * kind:
 * - "flag": 不带值的开关类参数（勾选则打开）
 * - "int" / "float": 带值参数，<input> 控制实际输入
 * defaultValue: 弹层中 <input> 初始化值（预解析失败时 fallback）。
 */
type BuildArgKind = "flag" | "int" | "float";
const BUILD_ARG_HELP: Array<{
  flag: string;
  kind: BuildArgKind;
  default: string;
  defaultValue: string;
  recommend: string;
  /** 说明文案：可含换行\n，弹层中用 white-space:pre-wrap 渲染。 */
  desc: string;
}> = [
  {
    flag: "--window",
    kind: "int",
    default: "20",
    defaultValue: "20",
    recommend: "20、30、充分覆盖一个动作",
    desc:
      "滑窗长度（帧数）。每个训练样本由连续 window 帧拼接而成。\n" +
      "  实际窗时长 = window × frame_period_ms（默认 20 × 50ms = 1.0 秒）。\n" +
      "\n" +
      "取值建议（按动作时长）：\n" +
      "• 短促手势（点击、敲击）：10~15（0.5~0.75 s）\n" +
      "• 常规手语字母/数字：20（1 s）  ← 默认值\n" +
      "• 慢动作或双手协作（如「你好」「谢谢」）：30~40（1.5~2 s）\n" +
      "\n" +
      "注意事项：\n" +
      "• 太短：动作不完整、类内方差大、模型难收敛\n" +
      "• 太长：内存占用 / 推理延迟高、训练样本量减少（受总帧数约束）、易过拟合\n" +
      "• 修改后必须重新跑切窗，已有 npy/CSV 不会自动更新",
  },
  {
    flag: "--stride",
    kind: "int",
    default: "10",
    defaultValue: "10",
    recommend: "10（即 50% 重叠）",
    desc:
      "滑窗步长（帧数）。两个相邻窗起点的间隔。\n" +
      "  重叠率 = 1 - stride/window；默认 10/20 → 50% 重叠。\n" +
      "\n" +
      "取值建议：\n" +
      "• window/2（默认）：50% 重叠，样本量与冗余的折中\n" +
      "• window/4：75% 重叠，数据稀缺时多采样\n" +
      "• 等于 window：0 重叠，样本独立性最强、量最少\n" +
      "\n" +
      "经验：\n" +
      "• 总样本数 ≈ (总帧数 - window) / stride + 1\n" +
      "• 重叠过高（>75%）会让 train/test 间样本高度相关、测试集失真\n" +
      "• 必须 ≤ window，否则切窗结果不连续",
  },
  {
    flag: "--flex-channel",
    kind: "int",
    default: "1",
    defaultValue: "1",
    recommend: "单手：1（食指）；双手：忽略此参数",
    desc:
      "【仅单手模式生效】选哪根手指作为单手主特征（每帧 1 维）。\n" +
      "  对应 raw.csv 中的 flex0..flex4 列；输出形状 (N, window, 1)。\n" +
      "\n" +
      "取值（手指映射，与硬件 ADC 通道对应）：\n" +
      "• 0 = 大拇指（thumb）\n" +
      "• 1 = 食指（index）        ← 推荐：弯曲幅度最大、信号 SNR 最高\n" +
      "• 2 = 中指（middle）\n" +
      "• 3 = 无名指（ring）\n" +
      "• 4 = 小指（pinky）\n" +
      "\n" +
      "⚠️ 双手模式（--bimanual）下本参数被完全忽略：\n" +
      "• 脚本自动取 26 通道全部特征（不能挑某一根手指）\n" +
      "• 通道排布（与端侧 printBimanualCsvRow 严格对齐）：\n" +
      "   ‑ ch  0~7 ：master（右手）IMU  ax,ay,az,gx,gy,gz,pitch,roll\n" +
      "   ‑ ch  8~12：master（右手）flex  0=拇指 1=食指 2=中指 3=无名指 4=小指\n" +
      "   ‑ ch 13~20：slave （左手）IMU  ax,ay,az,gx,gy,gz,pitch,roll\n" +
      "   ‑ ch 21~25：slave （左手）flex  0=拇指 1=食指 2=中指 3=无名指 4=小指\n" +
      "• 如需在双手数据上做单指实验，请自行下游切片 X[:, :, 9] 取右手食指",
  },
  {
    flag: "--frame-period-ms",
    kind: "int",
    default: "50",
    defaultValue: "50",
    recommend: "50（20 Hz）",
    desc:
      "采样周期 (ms)，必须与端侧 firmware 的实际帧率一致。\n" +
      "  典型对应关系：50 = 20 Hz、100 = 10 Hz、20 = 50 Hz。\n" +
      "\n" +
      "取值建议：\n" +
      "• LingxiGlove 默认固件：50 ms（20 Hz）  ← 不要随意更改\n" +
      "• 修改前先 flash 端侧对应固件并验证 timestamp_ms 实际间隔\n" +
      "\n" +
      "作用范围：\n" +
      "• 仅写入 EI CSV 第 1 列（timestamp 单调递增）\n" +
      "• 不参与切窗逻辑（切窗只看帧序号）\n" +
      "• 错填会导致 Edge Impulse 频谱分析时频率轴错位（FFT 频带漂移）",
  },
  {
    flag: "--test-ratio",
    kind: "float",
    default: "0.2",
    defaultValue: "0.2",
    recommend: "0.2 ~ 0.3",
    desc:
      "测试集占总样本比例（0..1，独占小数）。默认 0.2 = 80% 训练 / 20% 测试。\n" +
      "  按 session 内随机抽样切分（非按 session 整体），同 session 的窗口可能分布在两端。\n" +
      "\n" +
      "取值建议（按词汇量）：\n" +
      "• 类别数 ≤ 5     ：0.25~0.3，每类测试样本 ≥ 5 才有统计意义\n" +
      "• 类别数 5~10    ：0.2（默认）\n" +
      "• 类别数 > 10    ：0.15，多留训练样本\n" +
      "• 数据极少（<200 窗）：先 0.3 看 baseline，再考虑 K-fold",
  },
  {
    flag: "--seed",
    kind: "int",
    default: "42",
    defaultValue: "42",
    recommend: "42（保持可复现）",
    desc:
      "train/test 随机划分种子（int，传给 numpy.random.default_rng）。\n" +
      "\n" +
      "用法：\n" +
      "• 同 seed → 同一划分结果 → 可复现实验、定位错因\n" +
      "• 跨多 seed (e.g. 42、43、44) 取平均 → 消除划分偏差\n" +
      "• 调超参时固定 seed，最后报告时再用多 seed 求均值/方差\n" +
      "• 不影响模型训练侧的 seed（那个在训练脚本里另设）",
  },
  {
    flag: "--no-numpy",
    kind: "flag",
    default: "未启用",
    defaultValue: "",
    recommend: "绝大多数情况：不勾选",
    desc:
      "禁用 numpy npy 汇总写出（仅生成 EI CSV 给 Edge Impulse Studio）。\n" +
      "\n" +
      "输出对照：\n" +
      "• 默认（不勾）：输出 EI CSV + X_train.npy / X_test.npy / y_*.npy / labels.json\n" +
      "• 勾选       ：仅输出 EI CSV，跳过 npy 写盘\n" +
      "\n" +
      "何时勾选：\n" +
      "• 只走 Edge Impulse Studio 在线训练（CSV 上传即可）\n" +
      "• 磁盘紧张、不需要 numpy 副本\n" +
      "• 本地 PyTorch / TensorFlow / sklearn 训练 → 不要勾，需要 npy\n" +
      "• 双手 --bimanual 模式下本参数被强制忽略（双手仅出 npy 不出 EI CSV）",
  },
  {
    flag: "--bimanual",
    kind: "flag",
    default: "未启用",
    defaultValue: "",
    recommend: "双手动作训练时勾选；GUI 已具备自动注入",
    desc:
      "双手联合模式：消费 session_*_bimanual/ 目录，输出 26 通道融合样本。\n" +
      "\n" +
      "工作机制：\n" +
      "• MASTER（右手）本机帧 + SLAVE（左手）经 ESP-NOW 转发的帧\n" +
      "• 端侧已按 master 时间戳对齐后写入同一行（含 slave_age_ms）\n" +
      "• 脚本侧仅做 stale 过滤（见 --max-slave-age-ms）后切窗\n" +
      "\n" +
      "输出：\n" +
      "• X_bimanual_train.npy / X_bimanual_test.npy，shape = (N, window, 26)\n" +
      "• 通道顺序：8 master IMU + 5 master flex + 8 slave IMU + 5 slave flex\n" +
      "• 不输出 EI CSV（EI Studio 单文件不支持 26 通道）\n" +
      "\n" +
      "⚠️ 启用后：\n" +
      "• --flex-channel 失效（强制全 26 通道）\n" +
      "• --no-numpy 失效（必出 npy）\n" +
      "• 输入目录改为 session_*_bimanual/raw.csv（GUI 通过会话名后缀识别）",
  },
  {
    flag: "--max-slave-age-ms",
    kind: "int",
    default: "200",
    defaultValue: "200",
    recommend: "200（@20Hz 即 4 帧）",
    desc:
      "【仅 --bimanual 模式生效】slave 帧超时阈值 (ms)。\n" +
      "  raw.csv 每行的 slave_age_ms 字段记录「写盘时刻 - 最近一次收到 slave 帧时刻」。\n" +
      "\n" +
      "过滤逻辑：\n" +
      "• slave_age_ms < 0  → 还从未收到任何 slave 帧（slave 没上电 / 配对失败）\n" +
      "• slave_age_ms > 阈值 → slave 数据已过时（链路抖动 / slave 卡住）\n" +
      "• 命中任一条件 → 整行丢弃（stale_dropped 计数 +1）\n" +
      "\n" +
      "取值建议：\n" +
      "• 200（默认）：4 帧 @20Hz，正常链路下 stale_dropped 应该接近 0\n" +
      "• 500~1000 ：slave 偶发漏帧但基本在线时放宽\n" +
      "• 不建议 > 1000：超过 1 秒的 slave 数据已不能反映双手协作\n" +
      "\n" +
      "故障排查（rows=N stale_dropped=N、windows=0）：\n" +
      "1. slave 是否上电、电池电量是否正常\n" +
      "2. master/slave ESP-NOW MAC 配对是否一致（看 firmware config）\n" +
      "3. 录制时 slave 端 LED 状态（是否进入发送模式）\n" +
      "4. 录制时 GUI 是否报过 slave 离线/丢包警告",
  },
];

interface PipelineSettings {
  scriptPath: string;
  python: string;
  datasetRoot: string;
  extraArgsRaw: string; // 用户原始字符串，调用前 split
}

function loadSettings(defaults: PipelineSettings): PipelineSettings {
  try {
    const raw = localStorage.getItem(LS_KEY);
    if (!raw) return defaults;
    const parsed = JSON.parse(raw);
    return { ...defaults, ...parsed };
  } catch {
    return defaults;
  }
}

function saveSettings(s: PipelineSettings) {
  localStorage.setItem(LS_KEY, JSON.stringify(s));
}

export function PipelinePanel() {
  const labelNames = useCaptureStore((s) => s.labelNames);
  const [outRoot, setOutRoot] = useState<string>("");
  const [sessions, setSessions] = useState<SessionEntry[]>([]);
  const [settings, setSettings] = useState<PipelineSettings>(() =>
    loadSettings({
      scriptPath: "",
      python: "python3",
      datasetRoot: "",
      extraArgsRaw: "",
    })
  );
  const [eiKeyInput, setEiKeyInput] = useState("");
  const [eiKeySet, setEiKeySet] = useState(false);
  const [running, setRunning] = useState(false);
  const [logs, setLogs] = useState<PipelineProgress[]>([]);
  const [showArgsHelp, setShowArgsHelp] = useState(false);
  /**
   * 「额外参数」弹层中每个 flag 的当前状态。
   * - enabled: 是否勾选（应用后才会写进 extra_args）
   * - value:   带值参数的当前输入值；flag 类不使用
   * 打开弹层时从 extraArgsRaw 预解析初始化。
   */
  const [argsBuilder, setArgsBuilder] = useState<
    Record<string, { enabled: boolean; value: string }>
  >({});
  /** 选中的 session_id 集合（仅本会话内存，不持久化） */
  const [selectedIds, setSelectedIds] = useState<Set<string>>(new Set());
  /** 会话过滤模式：全部 / 仅选中 */
  const [sessionFilter, setSessionFilter] = useState<"all" | "selected">("all");
  const logBoxRef = useRef<HTMLDivElement | null>(null);

  // 初始化：拿 out_root + 默认脚本路径 + 检查 EI key + 拉 session 列表 + 订阅进度事件
  useEffect(() => {
    let unlisten: (() => void) | undefined;
    (async () => {
      try {
        const root = await getOutRoot();
        setOutRoot(root);
        // dataset_root / scriptPath 默认值补全（仅在未设置过时）
        const next = { ...settings };
        let dirty = false;
        if (!settings.datasetRoot) {
          next.datasetRoot = root.replace(/\/capture\/?$/, "/dataset");
          dirty = true;
        }
        if (!settings.scriptPath || LEGACY_SCRIPT_PATHS.includes(settings.scriptPath)) {
          try {
            next.scriptPath = await getDefaultScriptPath();
            dirty = true;
          } catch (e) {
            console.warn("getDefaultScriptPath:", e);
          }
        }
        if (dirty) {
          setSettings(next);
          saveSettings(next);
        }
        setSessions(await listSessions(root));
      } catch (e) {
        console.error("init pipeline panel:", e);
      }
      try {
        setEiKeySet(await hasEiKey());
      } catch (e) {
        console.error("hasEiKey:", e);
      }
      unlisten = await listen<PipelineProgress>("pipeline-progress", (ev) => {
        setLogs((prev) => {
          const next = prev.concat(ev.payload);
          return next.length > 500 ? next.slice(next.length - 500) : next;
        });
      });
    })();
    return () => {
      if (unlisten) unlisten();
    };
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, []);

  // 日志区自动滚到底部
  useEffect(() => {
    if (logBoxRef.current) {
      logBoxRef.current.scrollTop = logBoxRef.current.scrollHeight;
    }
  }, [logs.length]);

  const refreshSessions = useCallback(async () => {
    if (!outRoot) return;
    try {
      const list = await listSessions(outRoot);
      setSessions(list);
      // 过滤掉已不存在的选中项
      setSelectedIds((prev) => {
        const valid = new Set(list.map((s) => s.session_id));
        const next = new Set<string>();
        prev.forEach((id) => {
          if (valid.has(id)) next.add(id);
        });
        return next;
      });
    } catch (e) {
      alert(`刷新会话列表失败：${e}`);
    }
  }, [outRoot]);

  /** 单行复选框 toggle */
  const onToggleSelect = useCallback((sessionId: string) => {
    setSelectedIds((prev) => {
      const next = new Set(prev);
      if (next.has(sessionId)) next.delete(sessionId);
      else next.add(sessionId);
      return next;
    });
  }, []);

  /** 表头全选复选框 */
  const onToggleSelectAll = useCallback(() => {
    setSelectedIds((prev) => {
      if (prev.size === sessions.length) return new Set();
      return new Set(sessions.map((s) => s.session_id));
    });
  }, [sessions]);

  /** 删除选中的会话（含二次确认 + rm -rf）
   *
   * 注意：Tauri 2 的 macOS WebView 下原生 `window.confirm` / `alert`
   * 在部分场景下不阻塞（直接返回 true），必须走 plugin-dialog。
   */
  const onDeleteSelected = useCallback(async () => {
    if (selectedIds.size === 0) {
      await message("请先勾选要删除的会话", {
        title: "提示",
        kind: "info",
      });
      return;
    }
    const ids = Array.from(selectedIds);
    const preview = ids.slice(0, 5).join("\n  ");
    const more = ids.length > 5 ? `\n  …共 ${ids.length} 个` : "";
    const ok = await ask(
      `确认删除以下 ${ids.length} 个会话目录及其全部文件？此操作不可恢复。\n\n  ${preview}${more}`,
      {
        title: "删除会话",
        kind: "warning",
        okLabel: "删除",
        cancelLabel: "取消",
      },
    );
    if (!ok) return;
    let okCount = 0;
    const errs: string[] = [];
    for (const sid of ids) {
      try {
        await deleteSession(outRoot, sid);
        okCount += 1;
      } catch (e) {
        errs.push(`${sid}: ${e}`);
      }
    }
    if (errs.length > 0) {
      await message(
        `删除完成：成功 ${okCount}、失败 ${errs.length}\n\n${errs.join("\n")}`,
        { title: "删除结果", kind: "error" },
      );
    }
    setSelectedIds(new Set());
    await refreshSessions();
  }, [outRoot, refreshSessions, selectedIds]);

  const onSaveSettings = useCallback(() => {
    saveSettings(settings);
    alert("设置已保存");
  }, [settings]);

  /** 点「修改」选择会话输出根目录 */
  const onPickOutRoot = useCallback(async () => {
    try {
      const picked = await openDialog({
        directory: true,
        multiple: false,
        defaultPath: outRoot || undefined,
        title: "选择会话输出根目录",
      });
      if (!picked || typeof picked !== "string") return;
      await setOutRootApi(picked);
      setOutRoot(picked);
      // dataset_root 也同步重推荐为其同级的 dataset/ （仅在之前是默认推导值时）
      try {
        const next = await listSessions(picked);
        setSessions(next);
      } catch (e) {
        console.warn("listSessions after setOutRoot:", e);
        setSessions([]);
      }
    } catch (e) {
      alert(`修改 out_root 失败：${e}`);
    }
  }, [outRoot]);

  /** 点「选择」 dataset 输出根目录 */
  const onPickDatasetRoot = useCallback(async () => {
    try {
      const picked = await openDialog({
        directory: true,
        multiple: false,
        defaultPath: settings.datasetRoot || outRoot || undefined,
        title: "选择 dataset 输出根目录",
      });
      if (!picked || typeof picked !== "string") return;
      const next = { ...settings, datasetRoot: picked };
      setSettings(next);
      saveSettings(next);
    } catch (e) {
      alert(`选择 dataset 路径失败：${e}`);
    }
  }, [outRoot, settings]);

  /** 点「选择」 build_dataset.py 脚本路径（.py 文件） */
  const onPickScriptPath = useCallback(async () => {
    try {
      const picked = await openDialog({
        directory: false,
        multiple: false,
        defaultPath: settings.scriptPath || undefined,
        title: "选择 build_dataset.py 脚本",
        filters: [{ name: "Python 脚本", extensions: ["py"] }],
      });
      if (!picked || typeof picked !== "string") return;
      const next = { ...settings, scriptPath: picked };
      setSettings(next);
      saveSettings(next);
    } catch (e) {
      alert(`选择脚本路径失败：${e}`);
    }
  }, [settings]);

  /** 点「恢复默认」：拉发布包里内置的 build_dataset.py 路径 */
  const onResetScriptPath = useCallback(async () => {
    try {
      const def = await getDefaultScriptPath();
      const next = { ...settings, scriptPath: def };
      setSettings(next);
      saveSettings(next);
    } catch (e) {
      alert(`获取默认脚本路径失败：${e}`);
    }
  }, [settings]);

  const onSaveKey = useCallback(async () => {
    if (!eiKeyInput.trim()) {
      alert("请输入 EI API Key");
      return;
    }
    try {
      await setEiKey(eiKeyInput.trim());
      setEiKeySet(true);
      setEiKeyInput("");
      alert("EI API Key 已存入 Keychain");
    } catch (e) {
      alert(`保存失败：${e}`);
    }
  }, [eiKeyInput]);

  const onClearKey = useCallback(async () => {
    if (!confirm("确认删除已保存的 EI API Key?")) return;
    try {
      await deleteEiKey();
      setEiKeySet(false);
    } catch (e) {
      alert(`删除失败：${e}`);
    }
  }, []);

  /** 即将参与本次运行的 session 列表（按当前过滤模式）。
   *
   * 实际运行语义：
   * - 指定仅选中且有选中项   → 取子集
   * - 其他情况（含 后端 fallback） → 取全集
   */
  const targetSessions = useMemo(() => {
    if (sessionFilter === "selected" && selectedIds.size > 0) {
      return sessions.filter((s) => selectedIds.has(s.session_id));
    }
    return sessions;
  }, [sessions, sessionFilter, selectedIds]);

  /**
   * 是否自动注入 `--bimanual`。
   *
   * 触发条件（三者全中）：
   * 1. 待处理 session 非空
   * 2. 全部以 `_bimanual` 结尾（与 LingxiCapture bimanual writer 落盘名一致）
   * 3. 用户未在「额外参数」里显式写 --bimanual（避免重复注入）
   *
   * 混合场景（同时含单手 + 双手）不会自动注入，需用户「仅选中」+手动指定。
   */
  const autoBimanual = useMemo(() => {
    if (targetSessions.length === 0) return false;
    const allBimanual = targetSessions.every((s) =>
      s.session_id.endsWith("_bimanual"),
    );
    if (!allBimanual) return false;
    if (settings.extraArgsRaw.toLowerCase().includes("--bimanual")) return false;
    return true;
  }, [targetSessions, settings.extraArgsRaw]);

  const buildArgs: BuildDatasetArgs = useMemo(() => {
    const extra = settings.extraArgsRaw
      .split(/\s+/)
      .map((s) => s.trim())
      .filter(Boolean);
    if (autoBimanual) extra.unshift("--bimanual");
    const sessionIds =
      sessionFilter === "selected" ? Array.from(selectedIds) : undefined;
    return {
      in_root: outRoot,
      out_root: settings.datasetRoot,
      script_path: settings.scriptPath,
      python: settings.python || undefined,
      extra_args: extra,
      session_ids: sessionIds,
    };
  }, [autoBimanual, outRoot, selectedIds, sessionFilter, settings]);

  /** 打开「额外参数」弹层：从当前 extraArgsRaw 预解析填充 argsBuilder。
   *
   * 解析规则（宽容，与 Python argparse 对齐）：
   * - flag 类：tokens 中出现 → enabled=true
   * - value 类：tokens 中出现且下一个 token 不是以 -- 开头 → enabled=true, value=该 token
   * - 未出现：enabled=false, value=defaultValue
   *
   * 不识别的 token（如手写的 --foo bar）会被保留为 unknownArgs，apply 时拼在末尾。
   */
  const openArgsBuilder = useCallback(() => {
    const tokens = settings.extraArgsRaw
      .split(/\s+/)
      .map((s) => s.trim())
      .filter(Boolean);
    const next: Record<string, { enabled: boolean; value: string }> = {};
    for (const meta of BUILD_ARG_HELP) {
      const idx = tokens.indexOf(meta.flag);
      if (idx === -1) {
        next[meta.flag] = { enabled: false, value: meta.defaultValue };
      } else if (meta.kind === "flag") {
        next[meta.flag] = { enabled: true, value: "" };
      } else {
        const candidate = tokens[idx + 1];
        const useVal =
          candidate && !candidate.startsWith("--") ? candidate : meta.defaultValue;
        next[meta.flag] = { enabled: true, value: useVal };
      }
    }
    setArgsBuilder(next);
    setShowArgsHelp(true);
  }, [settings.extraArgsRaw]);

  /** 应用：按 BUILD_ARG_HELP 顺序拼装为字符串回填 extraArgsRaw，保留不识别的另封存。 */
  const onApplyArgs = useCallback(() => {
    const knownFlags = new Set(BUILD_ARG_HELP.map((m) => m.flag));
    // 保留原本字符串里不被 BUILD_ARG_HELP 覆盖的 token（含其跟随值）
    const oldTokens = settings.extraArgsRaw
      .split(/\s+/)
      .map((s) => s.trim())
      .filter(Boolean);
    const unknown: string[] = [];
    for (let i = 0; i < oldTokens.length; i++) {
      const t = oldTokens[i];
      if (knownFlags.has(t)) {
        // 跳过 known flag 及其跟随值（若 kind!=flag）
        const meta = BUILD_ARG_HELP.find((m) => m.flag === t)!;
        if (meta.kind !== "flag" && oldTokens[i + 1] && !oldTokens[i + 1].startsWith("--")) {
          i++;
        }
        continue;
      }
      unknown.push(t);
    }
    const out: string[] = [];
    for (const meta of BUILD_ARG_HELP) {
      const cur = argsBuilder[meta.flag];
      if (!cur?.enabled) continue;
      out.push(meta.flag);
      if (meta.kind !== "flag") {
        const v = (cur.value ?? "").trim();
        if (v) out.push(v);
      }
    }
    out.push(...unknown);
    setSettings({ ...settings, extraArgsRaw: out.join(" ") });
    setShowArgsHelp(false);
  }, [argsBuilder, settings]);

  const onRunBuild = useCallback(async () => {
    if (!outRoot || !settings.datasetRoot || !settings.scriptPath) {
      alert("请先填写 out_root / dataset_root / script_path");
      return;
    }
    setRunning(true);
    setLogs([]);
    try {
      const r = await runBuildDataset(buildArgs);
      alert(`build_dataset 结束：exit=${r.exit_code} ok=${r.ok}`);
    } catch (e) {
      alert(`build_dataset 失败：${e}`);
    } finally {
      setRunning(false);
    }
  }, [buildArgs, outRoot, settings]);

  const onRunUpload = useCallback(async () => {
    if (!eiKeySet) {
      alert("请先在「EI 设置」中填写 API Key");
      return;
    }
    if (!settings.datasetRoot) {
      alert("请先填写 dataset_root");
      return;
    }
    setRunning(true);
    try {
      const r = await uploadToEi(settings.datasetRoot);
      alert(`上传完成：${r.uploaded}/${r.total}（失败 ${r.failed}）`);
    } catch (e) {
      alert(`上传失败：${e}`);
    } finally {
      setRunning(false);
    }
  }, [eiKeySet, settings]);

  const onRunPipeline = useCallback(async () => {
    if (!outRoot || !settings.datasetRoot || !settings.scriptPath) {
      alert("请先填写 out_root / dataset_root / script_path");
      return;
    }
    if (!eiKeySet) {
      alert("请先在「EI 设置」中填写 API Key");
      return;
    }
    setRunning(true);
    setLogs([]);
    try {
      const r = await runPipeline(buildArgs, settings.datasetRoot);
      const upMsg = r.upload
        ? `上传 ${r.upload.uploaded}/${r.upload.total} (失败 ${r.upload.failed})`
        : "上传未执行（build 失败）";
      alert(`一键流水线完成：build=${r.build.ok} ${upMsg}`);
    } catch (e) {
      alert(`流水线失败：${e}`);
    } finally {
      setRunning(false);
    }
  }, [buildArgs, eiKeySet, outRoot, settings]);

  return (
    <div className="pipeline">
      {/* ==== Sessions ==== */}
      <div className="pipeline__card">
        <div className="pipeline__cardhead">
          <span>📁 会话列表</span>
          <div className="pipeline__cardhead-actions">
            <button
              type="button"
              className="btn btn--danger"
              onClick={onDeleteSelected}
              disabled={selectedIds.size === 0 || running}
              title="删除已勾选会话目录（不可恢复）"
            >
              删除选中{selectedIds.size > 0 ? `（${selectedIds.size}）` : ""}
            </button>
            <button type="button" className="btn btn--ghost" onClick={refreshSessions}>
              刷新
            </button>
          </div>
        </div>
        <div className="pipeline__path">
          <span>out_root: <code>{outRoot || "(未就绪)"}</code></span>
          <button
            type="button"
            className="btn btn--ghost"
            onClick={onPickOutRoot}
            style={{ marginLeft: 8 }}
            title="选择会话输出根目录（会持久化保存）"
          >
            修改…
          </button>
        </div>
        {sessions.length === 0 ? (
          <p className="pipeline__empty">暂无会话，先按 Space 开始一次录制。</p>
        ) : (
          <table className="pipeline__table">
            <thead>
              <tr>
                <th className="pipeline__chk-col">
                  <input
                    type="checkbox"
                    checked={
                      sessions.length > 0 && selectedIds.size === sessions.length
                    }
                    ref={(el) => {
                      if (el) {
                        el.indeterminate =
                          selectedIds.size > 0 &&
                          selectedIds.size < sessions.length;
                      }
                    }}
                    onChange={onToggleSelectAll}
                    aria-label="全选"
                  />
                </th>
                <th>session_id</th>
                <th>行数</th>
                <th>已打标</th>
                <th>Label 分布</th>
              </tr>
            </thead>
            <tbody>
              {sessions.map((s) => (
                <tr
                  key={s.session_id}
                  className={
                    selectedIds.has(s.session_id) ? "pipeline__row--selected" : undefined
                  }
                >
                  <td className="pipeline__chk-col">
                    <input
                      type="checkbox"
                      checked={selectedIds.has(s.session_id)}
                      onChange={() => onToggleSelect(s.session_id)}
                      aria-label={`选择 ${s.session_id}`}
                    />
                  </td>
                  <td>
                    <code>{s.session_id}</code>
                  </td>
                  <td>{s.rows}</td>
                  <td>
                    {s.labeled_rows}
                    {s.rows > 0 && (
                      <span className="pipeline__pct">
                        {" "}
                        ({((s.labeled_rows / s.rows) * 100).toFixed(0)}%)
                      </span>
                    )}
                  </td>
                  <td className="pipeline__labels">
                    {s.label_counts && s.label_counts.length > 0 ? (
                      formatLabelCounts(s.label_counts, labelNames).map(
                        (it, idx, arr) => (
                          <span
                            key={it.key}
                            className={
                              it.isUnlabeled
                                ? "pipeline__label-chip pipeline__label-chip--unlabeled"
                                : "pipeline__label-chip"
                            }
                            title={`label=${it.key}`}
                          >
                            {it.name}: {it.count}
                            {idx < arr.length - 1 ? " · " : ""}
                          </span>
                        ),
                      )
                    ) : (
                      <span className="pipeline__label-chip pipeline__label-chip--unlabeled">
                        —
                      </span>
                    )}
                  </td>
                </tr>
              ))}
            </tbody>
          </table>
        )}
      </div>

      {/* ==== Settings ==== */}
      <div className="pipeline__card">
        <div className="pipeline__cardhead">
          <span>⚙️ 流水线设置</span>
          <button type="button" className="btn btn--ghost" onClick={onSaveSettings}>
            保存
          </button>
        </div>
        <label className="pipeline__field">
          <span>build_dataset.py 路径</span>
          <div className="pipeline__keyrow">
            <input
              value={settings.scriptPath}
              onChange={(e) =>
                setSettings({ ...settings, scriptPath: e.target.value })
              }
            />
            <button
              type="button"
              className="btn btn--ghost"
              onClick={onPickScriptPath}
              title="选择 build_dataset.py 脚本文件"
            >
              选择…
            </button>
            <button
              type="button"
              className="btn btn--ghost"
              onClick={onResetScriptPath}
              title="恢复为发布包内置的默认脚本"
            >
              恢复默认
            </button>
          </div>
        </label>
        <label className="pipeline__field">
          <span>Python 解释器</span>
          <input
            value={settings.python}
            placeholder="python3"
            onChange={(e) => setSettings({ ...settings, python: e.target.value })}
          />
        </label>
        <label className="pipeline__field">
          <span>dataset 输出根</span>
          <div className="pipeline__keyrow">
            <input
              value={settings.datasetRoot}
              onChange={(e) =>
                setSettings({ ...settings, datasetRoot: e.target.value })
              }
            />
            <button
              type="button"
              className="btn btn--ghost"
              onClick={onPickDatasetRoot}
              title="选择 dataset 输出根目录"
            >
              选择…
            </button>
          </div>
        </label>
        <label className="pipeline__field">
          <span>
            额外参数（空格分隔）
            <button
              type="button"
              className="pipeline__help-btn"
              onClick={() => {
                if (showArgsHelp) setShowArgsHelp(false);
                else openArgsBuilder();
              }}
              title="可视化勾选参数"
              aria-label="参数构建器"
            >
              ?
            </button>
          </span>
          <input
            value={settings.extraArgsRaw}
            placeholder="--window 20 --stride 10 --flex-channel 1"
            onChange={(e) =>
              setSettings({ ...settings, extraArgsRaw: e.target.value })
            }
          />
        </label>
        {showArgsHelp && (
          // 注意：必须放在 <label> 外部。放在 label 内部时，浏览器会把
          // label 内任意点击当作「点击关联 input」，导致弹层内 button/checkbox
          // 点击被争夺/丢失 focus，表现为「随便点一下弹层就消失」。
          // onMouseDown / onClick 阻止冲泡，作为兄弟节点下的兄底防护。
          <div
            className="pipeline__argshelp"
            onMouseDown={(e) => e.stopPropagation()}
            onClick={(e) => e.stopPropagation()}
          >
            <div className="pipeline__argshelp-head">
              <strong>参数构建器</strong>
              <span className="pipeline__hint">
                勾选要启用的参数、必要时修改值，点「应用」自动填回输入框
              </span>
              <div className="pipeline__argshelp-actions">
                <button type="button" className="btn btn--ghost" onClick={() => setShowArgsHelp(false)}>
                  取消
                </button>
                <button type="button" className="btn" onClick={onApplyArgs}>
                  应用
                </button>
              </div>
            </div>
            <table>
              <thead>
                <tr>
                  <th>启用</th>
                  <th>参数</th>
                  <th>值</th>
                  <th>默认值</th>
                  <th>建议值</th>
                  <th>说明</th>
                </tr>
              </thead>
              <tbody>
                {BUILD_ARG_HELP.map((row) => {
                  const cur = argsBuilder[row.flag] ?? { enabled: false, value: row.defaultValue };
                  return (
                    <tr key={row.flag}>
                      <td className="pipeline__argshelp-enable">
                        <input
                          type="checkbox"
                          checked={cur.enabled}
                          onChange={(e) =>
                            setArgsBuilder((prev) => ({
                              ...prev,
                              [row.flag]: { ...cur, enabled: e.target.checked },
                            }))
                          }
                          aria-label={`启用 ${row.flag}`}
                        />
                      </td>
                      <td><code>{row.flag}</code></td>
                      <td className="pipeline__argshelp-value">
                        {row.kind === "flag" ? (
                          <span className="pipeline__hint">—</span>
                        ) : (
                          <input
                            type={row.kind === "int" || row.kind === "float" ? "number" : "text"}
                            step={row.kind === "float" ? "0.01" : "1"}
                            value={cur.value}
                            onChange={(e) =>
                              setArgsBuilder((prev) => ({
                                ...prev,
                                [row.flag]: { ...cur, value: e.target.value },
                              }))
                            }
                            disabled={!cur.enabled}
                            aria-label={`${row.flag} 值`}
                          />
                        )}
                      </td>
                      <td>{row.default}</td>
                      <td>{row.recommend}</td>
                      <td>{row.desc}</td>
                    </tr>
                  );
                })}
              </tbody>
            </table>
            <p className="pipeline__argshelp-foot">
              提示：<code>--in</code> / <code>--out</code> 已由上方「out_root」/「dataset 输出根」控制，无需重复填写。
            </p>
          </div>
        )}

        <div className="pipeline__divider" />

        <div className="pipeline__field">
          <span>
            EI API Key{" "}
            {eiKeySet ? (
              <em className="pipeline__ok">✓ 已存入 Keychain</em>
            ) : (
              <em className="pipeline__warn">未配置</em>
            )}
          </span>
          <div className="pipeline__keyrow">
            <input
              type="password"
              placeholder="ei_xxxxxxxx..."
              value={eiKeyInput}
              onChange={(e) => setEiKeyInput(e.target.value)}
            />
            <button type="button" className="btn" onClick={onSaveKey}>
              保存
            </button>
            {eiKeySet && (
              <button
                type="button"
                className="btn btn--danger"
                onClick={onClearKey}
              >
                清除
              </button>
            )}
          </div>
        </div>
      </div>

      {/* ==== Run ==== */}
      <div className="pipeline__card">
        <div className="pipeline__cardhead">
          <span>🚀 运行</span>
          <span className="pipeline__hint">
            建议先 build_dataset 检查通过，再走一键流水线
          </span>
        </div>
        <div className="pipeline__filter">
          <span>会话过滤：</span>
          <label>
            <input
              type="radio"
              name="sessionFilter"
              checked={sessionFilter === "all"}
              onChange={() => setSessionFilter("all")}
            />
            全部（{sessions.length}）
          </label>
          <label>
            <input
              type="radio"
              name="sessionFilter"
              checked={sessionFilter === "selected"}
              onChange={() => setSessionFilter("selected")}
              disabled={selectedIds.size === 0}
            />
            仅选中（{selectedIds.size}）
          </label>
          {sessionFilter === "selected" && selectedIds.size === 0 && (
            <span className="pipeline__warn">
              未勾选会话，运行时将回退为「全部」
            </span>
          )}
          {autoBimanual && (
            <span className="pipeline__hint pipeline__hint--ok">
              ✓ 已检测为双手数据，自动注入 --bimanual
            </span>
          )}
        </div>
        <div className="pipeline__buttons">
          <button
            type="button"
            className="btn"
            onClick={onRunBuild}
            disabled={running}
          >
            ① build_dataset
          </button>
          <button
            type="button"
            className="btn"
            onClick={onRunUpload}
            disabled={running || !eiKeySet}
          >
            ② upload to EI
          </button>
          <button
            type="button"
            className="btn btn--primary"
            onClick={onRunPipeline}
            disabled={running || !eiKeySet}
          >
            🚀 一键流水线（build + upload）
          </button>
          <button
            type="button"
            className="btn btn--ghost"
            onClick={() => setLogs([])}
            disabled={running}
          >
            清空日志
          </button>
        </div>

        <div ref={logBoxRef} className="pipeline__log">
          {logs.length === 0 ? (
            <div className="pipeline__empty">（暂无日志）</div>
          ) : (
            logs.map((l, i) => (
              <div key={i} className={"pipeline__logline pipeline__logline--" + l.stage}>
                <span className="pipeline__logstage">[{l.stage}]</span>
                {l.total > 0 && (
                  <span className="pipeline__logpct">
                    {l.current}/{l.total}
                  </span>
                )}
                <span className="pipeline__logmsg">{l.message}</span>
              </div>
            ))
          )}
        </div>
      </div>
    </div>
  );
}
