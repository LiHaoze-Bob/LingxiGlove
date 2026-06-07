/**
 * DeviceCalibrationCard — 单块板子的校准卡片
 *
 * 职责：
 *  - 绑定一个独立 alias（CAL_LEFT / CAL_RIGHT），不与 capture tab 的 left/right 冲突
 *  - 内嵌 ConnectBar 提供端口选择 + 连接 / 断开
 *  - 连接后展示：角色识别（master/slave）+ 校准状态徽章 + IMU/Flex 详情
 *  - 提供按钮：开始校准（IDLE 可点）/ 重新校准（已校准时） / 清除校准（二次确认）
 *
 * 数据来源：useCaptureStore.calibrationCards[alias]
 * 事件路由：connect_device / serial-role / calibration-event 都通过 alias 区分
 */
import { useCallback, useEffect, useMemo, useState } from "react";
import { ConnectBar } from "./ConnectBar";
import { CalibrationWizard } from "./CalibrationWizard";
import {
  clearCalibration,
  clearDeviceWifi,
  clearNvsRole,
  readCalibrationState,
  readDeviceInfo,
  sendChar,
  setDeviceRole,
  setPeerMac,
  setWifi,
} from "../api";
import { useCaptureStore } from "../store";
import {
  CAL_FLAG_FLEX,
  CAL_FLAG_IMU,
  type CfgInfo,
  type DeviceMeta,
  type SerialPortInfo,
} from "../types";

interface Props {
  alias: string;
  displayName: string;
  /** 当前系统串口列表 */
  ports: SerialPortInfo[];
  /** 刷新串口列表 */
  onPortsRefresh: () => Promise<void>;
  /** 当前所有设备元信息（用于读 status） */
  devices: DeviceMeta[];
  /** 设备状态变化时通知父级 refreshDevices */
  onDevicesChanged: () => Promise<void>;
  /**
   * 对端卡片报告的 self_mac（左卡传右卡的 self_mac，反之亦然）。
   * 用于在「对端 MAC」区做一致性校验徽章 + 一键互填。
   * 对端未连接 / 尚未读到 [CFG_INFO] 时为 null。
   */
  peerSelfMac: string | null;
}

function badge(flags: number): { label: string; cls: string } {
  const hasImu = (flags & CAL_FLAG_IMU) !== 0;
  const hasFlex = (flags & CAL_FLAG_FLEX) !== 0;
  if (hasImu && hasFlex) return { label: "已校准 ✓", cls: "cal-badge--ok" };
  if (!hasImu && !hasFlex) return { label: "未校准 ✗", cls: "cal-badge--err" };
  return {
    label: hasImu ? "部分校准 (仅 IMU)" : "部分校准 (仅 Flex)",
    cls: "cal-badge--warn",
  };
}

function formatBias(v: number): string {
  return v.toFixed(4);
}

/**
 * 把 firmware 的运行模式映射为「中文标签 + 徽章颜色 + 是否需要警告」。
 *
 * 设计目标：让校准 Tab 用户一眼分辨「现在是不是处于非默认模式」。
 * 命名遵循「正常 / 采集 / 指拼 / 测试」状态机风格，不与 UI 其它语义混用。
 * - recognize        → 绿色「正常模式」（默认态，识别 + TTS 正常）
 * - capture          → 红色「采集模式」（CSV 流；TTS / 识别已暂停）
 * - finger_spelling  → 黄色「指拼模式」（字母拼写训练态）
 * - accuracy_test    → 黄色「测试模式」（准确率验证态）
 * - null（老固件）   → 灰色「未知（老固件？）」
 */
function modeBadge(
  mode: string | null
): { label: string; cls: string; warn: boolean } {
  switch (mode) {
    case "recognize":
      return { label: "● 正常模式", cls: "cal-badge--ok", warn: false };
    case "capture":
      return { label: "● 采集模式（CSV 流）", cls: "cal-badge--err", warn: true };
    case "finger_spelling":
      return { label: "● 指拼模式", cls: "cal-badge--warn", warn: true };
    case "accuracy_test":
      return { label: "● 测试模式（准确率）", cls: "cal-badge--warn", warn: true };
    case null:
    case undefined:
      return { label: "○ 未知（老固件？）", cls: "cal-badge--unknown", warn: false };
    default:
      return { label: `● ${mode}`, cls: "cal-badge--warn", warn: true };
  }
}

export function DeviceCalibrationCard({
  alias,
  displayName,
  ports,
  onPortsRefresh,
  devices,
  onDevicesChanged,
  peerSelfMac,
}: Props) {
  const card = useCaptureStore((s) => s.calibrationCards[alias]);
  const setCalCardConnection = useCaptureStore((s) => s.setCalCardConnection);
  const openCalWizard = useCaptureStore((s) => s.openCalWizard);

  const [detailOpen, setDetailOpen] = useState(false);
  const [busy, setBusy] = useState(false);
  const [err, setErr] = useState<string | null>(null);

  const meta = useMemo(
    () => devices.find((d) => d.alias === alias),
    [devices, alias]
  );
  const isConnected = meta?.status === "connected";

  // 同步连接状态到 store（用于 wizard 判活）
  useEffect(() => {
    setCalCardConnection(alias, {
      connected: isConnected,
      port: meta?.port ?? null,
    });
  }, [alias, isConnected, meta?.port, setCalCardConnection]);

  // 连接成功后自动读一次 NVS 校准 + 设备配置（[CFG_INFO]）
  // 重连场景下 firmware 可能刚重启，boot banner 还在打印，单次发 info 容易丢，
  // 因此用 1.2s / 3.0s 双次发送做兜底。firmware 端处理 info 命令是幂等的。
  useEffect(() => {
    if (!isConnected) return;
    const t1 = setTimeout(() => {
      readCalibrationState(alias).catch((e) =>
        console.warn(`readCalibrationState(${alias}) failed:`, e)
      );
    }, 800); // 等 firmware 完成 boot banner 输出
    const t2 = setTimeout(() => {
      readDeviceInfo(alias).catch((e) =>
        console.warn(`readDeviceInfo(${alias}) failed:`, e)
      );
    }, 1200);
    // retry：兜底处理 firmware 刚重启 / boot 较慢导致首次 info 丢失
    const t3 = setTimeout(() => {
      readDeviceInfo(alias).catch(() => {
        /* 忽略：retry 失败不影响主流程 */
      });
    }, 3000);
    return () => {
      clearTimeout(t1);
      clearTimeout(t2);
      clearTimeout(t3);
    };
  }, [alias, isConnected]);

  const handleStart = useCallback(() => {
    if (!isConnected) return;
    openCalWizard(alias);
  }, [alias, isConnected, openCalWizard]);

  const handleRefresh = useCallback(async () => {
    if (!isConnected) return;
    setBusy(true);
    setErr(null);
    try {
      await readCalibrationState(alias);
    } catch (e) {
      setErr(String(e));
    } finally {
      setBusy(false);
    }
  }, [alias, isConnected]);

  const handleClear = useCallback(async () => {
    if (!isConnected) return;
    if (!window.confirm(`确认清除 ${displayName} 的 NVS 校准数据？`)) return;
    setBusy(true);
    setErr(null);
    try {
      await clearCalibration(alias);
    } catch (e) {
      setErr(String(e));
    } finally {
      setBusy(false);
    }
  }, [alias, displayName, isConnected]);

  /**
   * 强制把 firmware 拽回 RECOGNIZE 模式：发 'r' 单字符命令。
   * 用于「校准 Tab 显示当前是采集/指拼/测试模式」时的一键复位。
   * 发完 'r' 后多次轮询 [CFG_INFO] 验证恢复结果，避免单次 250ms 错过窗口。
   */
  const handleResetMode = useCallback(async () => {
    if (!isConnected) return;
    setBusy(true);
    setErr(null);
    try {
      await sendChar(alias, "r");
      // 100ms / 500ms / 1500ms 三轮重读，覆盖固件主循环不同节拍
      [100, 500, 1500].forEach((ms) => {
        setTimeout(() => {
          readDeviceInfo(alias).catch(() => {
            /* 静默：UI 仍会显示旧 mode 直到用户手动刷新 */
          });
        }, ms);
      });
    } catch (e) {
      setErr(String(e));
    } finally {
      setBusy(false);
    }
  }, [alias, isConnected]);

  /**
   * 切到 MODE_CAPTURE：发 'c' 单字符命令。
   * 仅作调试用——校准 Tab 主体职责是配置/校准；正式采集请用「数据采集 Tab」。
   * 注意：进入采集模式后 firmware 会暂停 TTS / 识别并开始 CSV 流，
   * UI 「模式」徽章会同步变红。再点「恢复正常」即可回到 recognize。
   */
  const handleEnterCapture = useCallback(async () => {
    if (!isConnected) return;
    if (
      !window.confirm(
        `进入采集模式后，${displayName} 将暂停 TTS / 识别并开始 CSV 流（仅供调试）。\n\n` +
          "正式数据采集请使用「数据采集 Tab」。\n\n确认进入？"
      )
    ) {
      return;
    }
    setBusy(true);
    setErr(null);
    try {
      await sendChar(alias, "c");
      // 100ms / 500ms / 1500ms 三轮重读，确保至少一次能读到新 mode
      [100, 500, 1500].forEach((ms) => {
        setTimeout(() => {
          readDeviceInfo(alias).catch(() => {});
        }, ms);
      });
    } catch (e) {
      setErr(String(e));
    } finally {
      setBusy(false);
    }
  }, [alias, displayName, isConnected]);

  const calInfo = card?.calInfo ?? null;
  const flags = calInfo?.flags ?? 0;
  const bg = badge(flags);
  const role = card?.role ?? null;
  const cfgMode = card?.cfgInfo?.mode ?? null;
  const mb = modeBadge(cfgMode);

  return (
    <div className="cal-card">
      <div className="cal-card__header">
        <span className="cal-card__title">{displayName}</span>
        <span className="cal-card__alias">alias: {alias}</span>
      </div>

      <div className="cal-card__connect">
        <ConnectBar
          alias={alias}
          displayName={displayName}
          status={meta?.status ?? "idle"}
          ports={ports}
          onPortsRefresh={onPortsRefresh}
          onChanged={onDevicesChanged}
        />
      </div>

      {isConnected && (
        <>
          <div className="cal-card__row">
            <span className="cal-card__field">角色</span>
            <span className={`cal-role cal-role--${role ?? "none"}`}>
              {role === "master"
                ? "MASTER"
                : role === "slave"
                ? "SLAVE"
                : role === "unknown"
                ? "未识别"
                : "等待 banner…"}
            </span>
          </div>

          {/* 当前固件运行模式（v3 [CFG_INFO] 字段）。
              进入校准 Tab 时一眼看清是不是采集/指拼/测试态——
              - 非 recognize：显示「🔧 恢复正常」（红色）
              - recognize    ：显示「⏺ 进入采集」（次按钮，仅调试用） */}
          <div className="cal-card__row">
            <span className="cal-card__field">模式</span>
            <span className={`cal-badge ${mb.cls}`}>{mb.label}</span>
            {mb.warn ? (
              <button
                type="button"
                className="btn btn--danger btn--sm"
                disabled={busy}
                onClick={handleResetMode}
                title="发送 'r' 让固件回到正常模式（恢复 TTS / 识别）"
              >
                🔧 恢复正常
              </button>
            ) : cfgMode === "recognize" ? (
              <button
                type="button"
                className="btn btn--ghost btn--sm"
                disabled={busy}
                onClick={handleEnterCapture}
                title="发送 'c' 进入采集模式（CSV 流 / 仅调试；正式采集请用数据采集 Tab）"
              >
                ⏺ 进入采集
              </button>
            ) : null}
          </div>

          <div className="cal-card__row">
            <span className="cal-card__field">校准状态</span>
            {calInfo ? (
              <span className={`cal-badge ${bg.cls}`}>{bg.label}</span>
            ) : (
              <span className="cal-badge cal-badge--unknown">读取中…</span>
            )}
            <button
              type="button"
              className="btn btn--ghost btn--sm"
              disabled={busy}
              onClick={handleRefresh}
              title="重新读取 NVS 校准状态"
            >
              ↻
            </button>
          </div>

          {calInfo && (
            <div className="cal-card__details">
              <button
                type="button"
                className="link-btn"
                onClick={() => setDetailOpen((v) => !v)}
              >
                {detailOpen ? "▼ 隐藏详情" : "▶ 查看详情"}
              </button>
              {detailOpen && (
                <div className="cal-card__details-body">
                  <div className="cal-detail-grp">
                    <div className="cal-detail-title">IMU bias</div>
                    <div className="cal-detail-row">
                      <span>accel</span>
                      <code>
                        [{formatBias(calInfo.accel_bias[0])},{" "}
                        {formatBias(calInfo.accel_bias[1])},{" "}
                        {formatBias(calInfo.accel_bias[2])}]
                      </code>
                    </div>
                    <div className="cal-detail-row">
                      <span>gyro</span>
                      <code>
                        [{formatBias(calInfo.gyro_bias[0])},{" "}
                        {formatBias(calInfo.gyro_bias[1])},{" "}
                        {formatBias(calInfo.gyro_bias[2])}]
                      </code>
                    </div>
                  </div>

                  <div className="cal-detail-grp">
                    <div className="cal-detail-title">Flex range</div>
                    <div className="cal-detail-row">
                      <span>min</span>
                      <code>[{calInfo.flex_min.join(", ")}]</code>
                    </div>
                    <div className="cal-detail-row">
                      <span>max</span>
                      <code>[{calInfo.flex_max.join(", ")}]</code>
                    </div>
                  </div>
                </div>
              )}
            </div>
          )}

          <div className="cal-card__btns">
            <button
              type="button"
              className="btn"
              disabled={busy || card?.wizardState === "running"}
              onClick={handleStart}
            >
              {flags === 0 ? "开始校准" : "重新校准"}
            </button>
            <button
              type="button"
              className="btn btn--danger"
              disabled={busy || flags === 0}
              onClick={handleClear}
            >
              清除校准
            </button>
          </div>

          {err && <div className="cal-card__err">⚠ {err}</div>}

          <DeviceConfigSection
            alias={alias}
            displayName={displayName}
            cfg={card?.cfgInfo ?? null}
            peerSelfMac={peerSelfMac}
          />
        </>
      )}

      {!isConnected && (
        <div className="cal-card__hint">
          请选择端口并连接以开始校准。校准数据写入板载 NVS，重启后保留。
        </div>
      )}

      {card?.wizardOpen && (
        <CalibrationWizard alias={alias} displayName={displayName} />
      )}
    </div>
  );
}

// ---------------- DeviceConfigSection ----------------

interface ConfigProps {
  alias: string;
  displayName: string;
  cfg: CfgInfo | null;
  /** 对端的 self_mac（即本端期望的 peer_mac）；null = 对端未连接或未读到 cfg */
  peerSelfMac: string | null;
}

/**
 * 比较两个 MAC 是否相等（忽略大小写）
 */
function macEq(a: string | null, b: string | null): boolean {
  if (!a || !b) return false;
  return a.toUpperCase() === b.toUpperCase();
}

/**
 * 设备配置折叠区：角色 / MAC / WiFi
 *
 * **重要**：所有写 NVS 的 firmware 命令都会触发设备 3s 后自动重启。
 * 因此每个写操作前都要 confirm("将自动重启设备")。
 */
function DeviceConfigSection({ alias, displayName, cfg, peerSelfMac }: ConfigProps) {
  const [open, setOpen] = useState(false);
  const [busy, setBusy] = useState(false);
  const [err, setErr] = useState<string | null>(null);
  const [peer, setPeer] = useState("");
  const [ssid, setSsid] = useState("");
  const [pwd, setPwd] = useState("");
  // WiFi 密码可见性切换：默认隐藏（圆点），点眼睛图标后明文显示
  const [pwdVisible, setPwdVisible] = useState(false);

  // cfg 更新时把当前 SSID / peer_mac 反填到输入框（方便就地编辑）
  // 反填策略：仅当输入框为空（用户未编辑）才填入；用户一旦键入就不再覆盖
  // 避免「打字打到一半被外部 cfg 刷新覆盖」
  useEffect(() => {
    if (cfg?.ssid) setSsid((cur) => (cur === "" ? cfg.ssid : cur));
  }, [cfg?.ssid]);
  useEffect(() => {
    if (cfg?.peer_mac) setPeer((cur) => (cur === "" ? cfg.peer_mac! : cur));
  }, [cfg?.peer_mac]);
  useEffect(() => {
    if (cfg?.wifi_pwd) setPwd((cur) => (cur === "" ? cfg.wifi_pwd! : cur));
  }, [cfg?.wifi_pwd]);

  const wrap = useCallback(
    async (label: string, action: () => Promise<void>) => {
      if (
        !window.confirm(
          `${displayName}: ${label}\n\n执行后设备将自动重启 (~3s)，确认继续？`
        )
      ) {
        return;
      }
      setBusy(true);
      setErr(null);
      try {
        await action();
      } catch (e) {
        setErr(String(e));
      } finally {
        setBusy(false);
      }
    },
    [displayName]
  );

  const handleSwitchRole = useCallback(() => {
    if (!cfg) return;
    const next = cfg.role === "master" ? "slave" : "master";
    wrap(`切换角色为 ${next.toUpperCase()}`, () => setDeviceRole(alias, next));
  }, [alias, cfg, wrap]);

  const handleSetPeer = useCallback(() => {
    const trimmed = peer.trim().toUpperCase();
    if (!trimmed) return;
    wrap(`设置对端 MAC = ${trimmed}`, () => setPeerMac(alias, trimmed));
  }, [alias, peer, wrap]);

  /** 把对端 self_mac 一键填入 input（不写 NVS，等用户点「设置」）。 */
  const handleFillPeerFromOther = useCallback(() => {
    if (!peerSelfMac) return;
    setPeer(peerSelfMac.toUpperCase());
  }, [peerSelfMac]);

  /** 错配/未配置 → 直接调用 setPeerMac 写 NVS（带二次确认），跳过 input。 */
  const handleAutoFixPeer = useCallback(() => {
    if (!peerSelfMac) return;
    const target = peerSelfMac.toUpperCase();
    wrap(`一键同步对端 MAC = ${target}`, () => setPeerMac(alias, target));
  }, [alias, peerSelfMac, wrap]);

  const handleClearNvs = useCallback(() => {
    wrap("清除 NVS 角色 / 对端 MAC（恢复编译默认）", () => clearNvsRole(alias));
  }, [alias, wrap]);

  const handleSetWifi = useCallback(() => {
    if (!ssid.trim()) {
      setErr("SSID 不能为空");
      return;
    }
    wrap(`设置 WiFi: ${ssid}`, () => setWifi(alias, ssid.trim(), pwd));
  }, [alias, ssid, pwd, wrap]);

  const handleClearWifi = useCallback(() => {
    wrap("清除 WiFi NVS 凭据", () => clearDeviceWifi(alias));
  }, [alias, wrap]);

  const handleRefresh = useCallback(async () => {
    setBusy(true);
    setErr(null);
    try {
      await readDeviceInfo(alias);
    } catch (e) {
      setErr(String(e));
    } finally {
      setBusy(false);
    }
  }, [alias]);

  return (
    <div className="cal-card__cfg">
      <button
        type="button"
        className="link-btn"
        onClick={() => setOpen((v) => !v)}
      >
        {open ? "▼ 设备配置（角色 / MAC / WiFi）" : "▶ 设备配置（角色 / MAC / WiFi）"}
      </button>

      {open && (
        <div className="cal-cfg-body">
          <div className="cal-cfg-warn">
            ⚠ 以下任何修改都会触发设备 <b>自动重启 (~3s)</b>。
          </div>

          {/* 当前模式 + 刷新（显示固件 g_runMode：recognize/capture/...） */}
          <div className="cal-cfg-row">
            <span className="cal-card__field">当前</span>
            {cfg ? (
              <span className={`cal-badge ${modeBadge(cfg.mode ?? null).cls}`}>
                {modeBadge(cfg.mode ?? null).label}
              </span>
            ) : (
              <span className="cal-cfg-current cal-cfg-current--muted">
                读取中…
              </span>
            )}
            <button
              type="button"
              className="btn btn--ghost btn--sm"
              disabled={busy}
              onClick={handleRefresh}
              title="重新读取 [CFG_INFO]"
            >
              ↻
            </button>
          </div>

          {/* 本机 MAC + 对端 MAC（人读概览） */}
          {cfg && (
            <div className="cal-cfg-row" style={{ flexWrap: "wrap" }}>
              <span className="cal-card__field">本机 MAC</span>
              <code>{cfg.self_mac}</code>
              <span className="cal-card__field" style={{ marginLeft: 8 }}>
                对端
              </span>
              <code>{cfg.peer_mac ?? "未设置"}</code>
            </div>
          )}

          {/* WiFi：SSID / 状态 / IP / RSSI */}
          {cfg && (
            <div className="cal-cfg-row" style={{ flexWrap: "wrap" }}>
              <span className="cal-card__field">WiFi</span>
              <span
                className={
                  "cal-badge " +
                  (cfg.wifi_connected
                    ? "cal-badge--ok"
                    : "cal-badge--unknown")
                }
              >
                {cfg.wifi_connected ? "● 已连接" : "○ 未连接"}
              </span>
              <code>{cfg.ssid || "(SSID 空)"}</code>
              {cfg.wifi_connected && cfg.ip && (
                <>
                  <span className="cal-card__field" style={{ marginLeft: 8 }}>
                    IP
                  </span>
                  <code>{cfg.ip}</code>
                </>
              )}
              {cfg.wifi_connected && cfg.rssi !== null && (
                <>
                  <span className="cal-card__field" style={{ marginLeft: 8 }}>
                    RSSI
                  </span>
                  <code>{cfg.rssi} dBm</code>
                </>
              )}
            </div>
          )}

          {/* 角色 */}
          <div className="cal-cfg-grp">
            <div className="cal-cfg-grp-title">角色</div>
            <div className="cal-cfg-row">
              <button
                type="button"
                className="btn btn--sm"
                disabled={busy || !cfg}
                onClick={handleSwitchRole}
              >
                切换为 {cfg?.role === "master" ? "SLAVE" : "MASTER"}
              </button>
              <button
                type="button"
                className="btn btn--sm btn--danger"
                disabled={busy}
                onClick={handleClearNvs}
                title="清除 NVS 中的角色 / 对端 MAC，恢复 build_opt.h 默认"
              >
                清除 NVS（恢复编译默认）
              </button>
            </div>
          </div>

          {/* 对端 MAC */}
          <div className="cal-cfg-grp">
            <div className="cal-cfg-grp-title">对端 MAC</div>

            {/* 一致性徽章：基于 cfg.peer_mac vs 对端 self_mac 的实时校验 */}
            {(() => {
              if (!cfg) return null;
              if (!peerSelfMac) {
                return (
                  <div className="cal-cfg-row">
                    <span className="cal-badge cal-badge--unknown">
                      对端未连接，无法校验
                    </span>
                  </div>
                );
              }
              if (!cfg.peer_mac) {
                return (
                  <div className="cal-cfg-row">
                    <span className="cal-badge cal-badge--warn">
                      ⚠ 未配置对端 MAC
                    </span>
                    <button
                      type="button"
                      className="btn btn--sm"
                      disabled={busy}
                      onClick={handleAutoFixPeer}
                      title={`一键写入 NVS：${peerSelfMac.toUpperCase()}`}
                    >
                      ✨ 自动配置 = {peerSelfMac.toUpperCase()}
                    </button>
                  </div>
                );
              }
              if (macEq(cfg.peer_mac, peerSelfMac)) {
                return (
                  <div className="cal-cfg-row">
                    <span className="cal-badge cal-badge--ok">
                      ✓ 对端 MAC 一致
                    </span>
                  </div>
                );
              }
              return (
                <div className="cal-cfg-row">
                  <span className="cal-badge cal-badge--err">
                    ⚠ 对端 MAC 错配
                  </span>
                  <button
                    type="button"
                    className="btn btn--sm btn--danger"
                    disabled={busy}
                    onClick={handleAutoFixPeer}
                    title={`当前 NVS 中 peer=${cfg.peer_mac}，但对端 self=${peerSelfMac.toUpperCase()}`}
                  >
                    🔧 一键修复 = {peerSelfMac.toUpperCase()}
                  </button>
                </div>
              );
            })()}

            <div className="cal-cfg-row">
              <input
                type="text"
                className="cal-cfg-input"
                placeholder="AA:BB:CC:DD:EE:FF"
                value={peer}
                onChange={(e) => setPeer(e.target.value)}
                disabled={busy}
                spellCheck={false}
              />
              <button
                type="button"
                className="btn btn--ghost btn--sm"
                disabled={busy || !peerSelfMac}
                onClick={handleFillPeerFromOther}
                title={
                  peerSelfMac
                    ? `填入对端 self_mac: ${peerSelfMac.toUpperCase()}`
                    : "对端未连接"
                }
              >
                ⇆ 用对端
              </button>
              <button
                type="button"
                className="btn btn--sm"
                disabled={busy || peer.trim().length !== 17}
                onClick={handleSetPeer}
              >
                设置
              </button>
            </div>
          </div>

          {/* WiFi */}
          <div className="cal-cfg-grp">
            <div className="cal-cfg-grp-title">WiFi 凭据</div>
            <div className="cal-cfg-row">
              <input
                type="text"
                className="cal-cfg-input"
                placeholder="SSID（不含空格）"
                value={ssid}
                onChange={(e) => setSsid(e.target.value)}
                disabled={busy}
                spellCheck={false}
              />
              <input
                type={pwdVisible ? "text" : "password"}
                className="cal-cfg-input"
                placeholder="密码"
                value={pwd}
                onChange={(e) => setPwd(e.target.value)}
                disabled={busy}
                spellCheck={false}
                autoComplete="off"
              />
              <button
                type="button"
                className="btn btn--ghost btn--sm"
                disabled={busy}
                onClick={() => setPwdVisible((v) => !v)}
                title={pwdVisible ? "隐藏密码" : "显示密码"}
                aria-label={pwdVisible ? "隐藏密码" : "显示密码"}
              >
                {pwdVisible ? "🙈" : "👁"}
              </button>
              <button
                type="button"
                className="btn btn--sm"
                disabled={busy || !ssid.trim()}
                onClick={handleSetWifi}
              >
                设置
              </button>
              <button
                type="button"
                className="btn btn--sm btn--danger"
                disabled={busy}
                onClick={handleClearWifi}
              >
                清除
              </button>
            </div>
          </div>

          {err && <div className="cal-card__err">⚠ {err}</div>}
        </div>
      )}
    </div>
  );
}
