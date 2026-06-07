/**
 * 全局状态（zustand）
 *
 * 仅放**触发渲染**的状态：currentLabel / sessionInfo / lastSummary / captureFlow / labelNames
 *
 * **不放 frame ring buffer**：每秒 40+ 帧若进 React state 会卡 UI。
 * 帧数据走 useRef + uPlot.setData 绕开 React。
 */
import { create } from "zustand";
import {
  DEFAULT_LABEL_NAMES,
  LABEL_STORAGE_KEY,
  type CalCardWizardState,
  type CalInfo,
  type CalProgress,
  type CfgInfo,
  type SessionInfo,
  type SessionSummary,
} from "./types";
import {
  getLabel as apiGetLabel,
  getSessionInfo as apiGetSessionInfo,
  pauseSession as apiPauseSession,
  resumeSession as apiResumeSession,
  setLabel as apiSetLabel,
  startSession as apiStartSession,
  stopSession as apiStopSession,
  sendChar,
} from "./api";

const MASTER_ALIAS = "left";

/**
 * 当前活跃倒计时的取消句柄（module-level 闭包变量）。
 *
 * 由 startCountdownFlow 设置，cancelCountdown / GO 后置 null。
 * 同一时刻只允许一个倒计时（防 ABA：startCountdownFlow 已守 captureFlow==='IDLE'）。
 */
let countdownCancelImpl: (() => void) | null = null;

const EMPTY_SESSION: SessionInfo = {
  recording: false,
  paused: false,
  session_id: null,
  rows_per_device: {},
};

/**
 * Day 5 采集状态机（前端）
 *
 * - DISCONNECTED: 没有任何 master 连接
 * - HANDSHAKING:  连上但还没收到 [配置] 角色 banner
 * - IDLE:         master 角色已确认，等待用户按 Space
 * - COUNTDOWN:    倒计时 3 → 2 → 1 → GO（每秒一拍）
 * - RECORDING:    已发 'c'，正在写盘
 * - FINISHING:    已发 'r'，等 stop_session 返回
 */
export type CaptureFlow =
  | "DISCONNECTED"
  | "HANDSHAKING"
  | "IDLE"
  | "COUNTDOWN"
  | "RECORDING"
  | "FINISHING";

/** 板子角色检测结果（来自 serial-role 事件） */
export type DeviceRole = "master" | "slave" | "unknown" | null;

// ---------------- Day 6: 校准 Tab ----------------

/** 顶层 Tab — 数据采集 / 设备校准（互斥）*/
export type ActiveTab = "capture" | "calibration";

/**
 * 校准 Tab 中单块板子的 UI 状态
 *
 * port / connected / role 由 ConnectBar + serial-role 事件维护。
 * calInfo 由 [CAL_INFO] marker 注入，是 NVS 内容的快照。
 * progress 是最近一次进度 marker，wizard 据此驱动状态机。
 */
export interface CalCardState {
  port: string | null;
  connected: boolean;
  role: DeviceRole;
  /** 最近一次 [CAL_INFO] 解析结果；null 表示尚未读到 NVS */
  calInfo: CalInfo | null;
  /** 最近一次 [CFG_INFO] 解析结果；null 表示尚未读到（重启或刚连接） */
  cfgInfo: CfgInfo | null;
  wizardState: CalCardWizardState;
  /** 最近一次 [CAL] 进度 marker（wizard 内部状态机驱动） */
  lastProgress: CalProgress | null;
  /** 最近一次错误原因（reason 字段），用于 wizard error 视图 */
  lastError: string | null;
  /** wizard 弹窗是否打开 */
  wizardOpen: boolean;
}

function emptyCalCard(): CalCardState {
  return {
    port: null,
    connected: false,
    role: null,
    calInfo: null,
    cfgInfo: null,
    wizardState: "idle",
    lastProgress: null,
    lastError: null,
    wizardOpen: false,
  };
}

/** 加载 localStorage 中的 label 名（容错回退默认值） */
function loadLabelNames(): string[] {
  try {
    const raw = localStorage.getItem(LABEL_STORAGE_KEY);
    if (!raw) return [...DEFAULT_LABEL_NAMES];
    const parsed = JSON.parse(raw);
    if (!Array.isArray(parsed) || parsed.length !== 10) {
      return [...DEFAULT_LABEL_NAMES];
    }
    return parsed.map((v, i) =>
      typeof v === "string" && v.trim() ? v : DEFAULT_LABEL_NAMES[i]
    );
  } catch {
    return [...DEFAULT_LABEL_NAMES];
  }
}

function persistLabelNames(names: string[]) {
  try {
    localStorage.setItem(LABEL_STORAGE_KEY, JSON.stringify(names));
  } catch (e) {
    console.warn("persist labelNames failed:", e);
  }
}

interface CaptureState {
  /** 当前 label，-1 = unlabeled */
  currentLabel: number;
  /** 设置 label：先调 Rust，再更新 store；失败回滚不切换 */
  setCurrentLabel: (label: number) => Promise<void>;
  /** 后台已成为权威：从 Rust 拉取（启动时调用一次） */
  syncLabelFromBackend: () => Promise<void>;

  /** 录制状态（1Hz 同步） */
  sessionInfo: SessionInfo;
  refreshSessionInfo: () => Promise<void>;

  startSession: () => Promise<string>;
  pauseSession: () => Promise<void>;
  resumeSession: () => Promise<void>;
  stopSession: () => Promise<SessionSummary>;

  /** 最近一次会话结束摘要（前端 toast 展示） */
  lastSummary: SessionSummary | null;
  clearSummary: () => void;

  // ---------------- Day 5: 采集状态机 ----------------
  /** 主控板（master）的角色识别结果 */
  masterRole: DeviceRole;
  setMasterRole: (role: DeviceRole) => void;

  /** 当前采集流状态机 */
  captureFlow: CaptureFlow;
  setCaptureFlow: (flow: CaptureFlow) => void;

  /** 倒计时数字（3 → 2 → 1 → 0=GO） */
  countdown: number;
  setCountdown: (n: number) => void;

  /** 录制开始时间戳（performance.now()） */
  recordingStartedAt: number;
  setRecordingStartedAt: (t: number) => void;

  // ---------------- Day 5: label 名表（用户可编辑） ----------------
  /** 10 个 label 的展示名（index 0..9） */
  labelNames: string[];
  /** 重命名某个 label（持久化到 localStorage） */
  renameLabel: (index: number, name: string) => void;
  /** 重置为默认 label 名 */
  resetLabelNames: () => void;

  // ---------------- Day 5: 录制流编排（key + toolbar 共用） ----------------
  /** IDLE → 启动 3-2-1 倒计时 → GO 后发 'c' + startSession → RECORDING */
  startCountdownFlow: () => void;
  /** COUNTDOWN 中取消，回 IDLE */
  cancelCountdown: () => void;
  /** RECORDING / FINISHING → 发 'r' + stopSession → IDLE */
  stopRecordingFlow: () => Promise<void>;

  // ---------------- Day 6: 校准 Tab ----------------
  /** 顶层 Tab 切换 */
  activeTab: ActiveTab;
  setActiveTab: (tab: ActiveTab) => void;

  /** CAL_LEFT / CAL_RIGHT 两个独立 alias 的 UI 状态 */
  calibrationCards: Record<string, CalCardState>;
  /** 由 ConnectBar 调用，更新连接元信息 */
  setCalCardConnection: (
    alias: string,
    patch: Partial<Pick<CalCardState, "port" | "connected">>
  ) => void;
  /** serial-role 事件路由：更新 role */
  setCalCardRole: (alias: string, role: DeviceRole) => void;
  /** [CAL_INFO] 事件路由：更新 calInfo */
  setCalCardInfo: (alias: string, info: CalInfo) => void;
  /** [CFG_INFO] 事件路由：更新 cfgInfo */
  setCalCardCfg: (alias: string, cfg: CfgInfo) => void;
  /** [CAL] 进度事件路由：更新 lastProgress + 自动维护 wizardState/lastError */
  applyCalProgress: (alias: string, progress: CalProgress) => void;
  /** wizard 打开/关闭（不影响串口连接） */
  openCalWizard: (alias: string) => void;
  closeCalWizard: (alias: string) => void;
  /** 用户点重试或开始校准时清空错误标记 */
  resetCalCardError: (alias: string) => void;
}

export const useCaptureStore = create<CaptureState>((set, get) => ({
  currentLabel: -1,

  setCurrentLabel: async (label: number) => {
    try {
      await apiSetLabel(label);
      set({ currentLabel: label });
    } catch (e) {
      console.error("setLabel failed:", e);
      // 回滚：从后端再读一次
      try {
        const real = await apiGetLabel();
        set({ currentLabel: real });
      } catch {
        // 忽略二次失败
      }
    }
  },

  syncLabelFromBackend: async () => {
    try {
      const l = await apiGetLabel();
      set({ currentLabel: l });
    } catch (e) {
      console.error("getLabel failed:", e);
    }
  },

  sessionInfo: EMPTY_SESSION,

  refreshSessionInfo: async () => {
    try {
      const info = await apiGetSessionInfo();
      set({ sessionInfo: info });
    } catch (e) {
      console.error("getSessionInfo failed:", e);
    }
  },

  startSession: async () => {
    const id = await apiStartSession();
    await get().refreshSessionInfo();
    return id;
  },

  pauseSession: async () => {
    await apiPauseSession();
    await get().refreshSessionInfo();
  },

  resumeSession: async () => {
    await apiResumeSession();
    await get().refreshSessionInfo();
  },

  stopSession: async () => {
    const summary = await apiStopSession();
    set({ lastSummary: summary });
    await get().refreshSessionInfo();
    return summary;
  },

  lastSummary: null,
  clearSummary: () => set({ lastSummary: null }),

  // ---------------- Day 5 ----------------
  masterRole: null,
  setMasterRole: (role) => set({ masterRole: role }),

  captureFlow: "DISCONNECTED",
  setCaptureFlow: (flow) => set({ captureFlow: flow }),

  countdown: 0,
  setCountdown: (n) => set({ countdown: n }),

  recordingStartedAt: 0,
  setRecordingStartedAt: (t) => set({ recordingStartedAt: t }),

  labelNames: loadLabelNames(),
  renameLabel: (index, name) => {
    if (index < 0 || index > 9) return;
    const trimmed = name.trim();
    if (!trimmed) return;
    const next = [...get().labelNames];
    next[index] = trimmed;
    persistLabelNames(next);
    set({ labelNames: next });
  },
  resetLabelNames: () => {
    const next = [...DEFAULT_LABEL_NAMES];
    persistLabelNames(next);
    set({ labelNames: next });
  },

  // ---------------- Day 5: 录制流编排实现 ----------------
  startCountdownFlow: () => {
    const flow = get().captureFlow;
    if (flow !== "IDLE") return; // 防 ABA
    set({ captureFlow: "COUNTDOWN", countdown: 3 });
    const timers: ReturnType<typeof setTimeout>[] = [];
    let cancelled = false;

    // cancelCountdown 通过 set 把 captureFlow 切回 IDLE 并清 timers
    const cancel = () => {
      if (cancelled) return;
      cancelled = true;
      timers.forEach(clearTimeout);
    };
    // 把 cancel 挂在闭包里，cancelCountdown action 会调它
    countdownCancelImpl = cancel;

    timers.push(
      setTimeout(() => {
        if (!cancelled) set({ countdown: 2 });
      }, 1000)
    );
    timers.push(
      setTimeout(() => {
        if (!cancelled) set({ countdown: 1 });
      }, 2000)
    );
    timers.push(
      setTimeout(() => {
        if (!cancelled) set({ countdown: 0 }); // GO!
      }, 3000)
    );
    timers.push(
      setTimeout(async () => {
        if (cancelled) return;
        countdownCancelImpl = null;
        try {
          await sendChar(MASTER_ALIAS, "c");
          await new Promise((res) => setTimeout(res, 200));
          await get().startSession();
          set({
            recordingStartedAt: performance.now(),
            captureFlow: "RECORDING",
          });
        } catch (err) {
          console.error("start recording failed:", err);
          alert("启动录制失败：" + String(err));
          set({ captureFlow: "IDLE", countdown: 0 });
        }
      }, 3100)
    );
  },

  cancelCountdown: () => {
    if (countdownCancelImpl) {
      countdownCancelImpl();
      countdownCancelImpl = null;
    }
    if (get().captureFlow === "COUNTDOWN") {
      set({ captureFlow: "IDLE", countdown: 0 });
    }
  },

  stopRecordingFlow: async () => {
    const flow = get().captureFlow;
    if (flow !== "RECORDING") return;
    set({ captureFlow: "FINISHING" });
    try {
      await sendChar(MASTER_ALIAS, "r");
    } catch (err) {
      console.warn("send 'r' failed (board may be off):", err);
    }
    try {
      await get().stopSession();
    } catch (err) {
      console.error("stopSession failed:", err);
      alert("停止会话失败：" + String(err));
    }
    set({ recordingStartedAt: 0, captureFlow: "IDLE" });
  },

  // ---------------- Day 6: 校准 Tab ----------------
  activeTab: "capture",
  setActiveTab: (tab) => set({ activeTab: tab }),

  calibrationCards: {
    CAL_LEFT: emptyCalCard(),
    CAL_RIGHT: emptyCalCard(),
  },

  setCalCardConnection: (alias, patch) => {
    const cur = get().calibrationCards[alias] ?? emptyCalCard();
    const next: CalCardState = { ...cur, ...patch };
    // 断开时同时清掉 role / wizardState / cfgInfo
    //   - cfgInfo 必须清空：避免重连/重启后 UI 仍显示旧 peer_mac/ip/rssi，
    //     用户会误以为"设置没生效"。重连后 readDeviceInfo 会重新拉一份。
    //   - calInfo 保留：NVS 里的校准只要没 'cal clear' 就还是上次的值，避免闪烁
    if (patch.connected === false) {
      next.role = null;
      next.wizardState = "idle";
      next.lastProgress = null;
      next.cfgInfo = null;
    }
    set({
      calibrationCards: { ...get().calibrationCards, [alias]: next },
    });
  },

  setCalCardRole: (alias, role) => {
    const cur = get().calibrationCards[alias] ?? emptyCalCard();
    set({
      calibrationCards: {
        ...get().calibrationCards,
        [alias]: { ...cur, role },
      },
    });
  },

  setCalCardInfo: (alias, info) => {
    const cur = get().calibrationCards[alias] ?? emptyCalCard();
    set({
      calibrationCards: {
        ...get().calibrationCards,
        [alias]: { ...cur, calInfo: info },
      },
    });
  },

  setCalCardCfg: (alias, cfg) => {
    const cur = get().calibrationCards[alias] ?? emptyCalCard();
    set({
      calibrationCards: {
        ...get().calibrationCards,
        [alias]: { ...cur, cfgInfo: cfg },
      },
    });
  },

  applyCalProgress: (alias, progress) => {
    const cur = get().calibrationCards[alias] ?? emptyCalCard();
    let wizardState: CalCardWizardState = cur.wizardState;
    let lastError: string | null = cur.lastError;
    // 任何 stage=overall phase=start → running
    if (progress.stage === "overall" && progress.phase === "start") {
      wizardState = "running";
      lastError = null;
    }
    // overall done ok=1 → idle，ok=0 → error
    if (progress.stage === "overall" && progress.phase === "done") {
      if (progress.ok === false) {
        wizardState = "error";
        lastError = progress.reason ?? "unknown_error";
      } else {
        wizardState = "idle";
      }
    }
    // 阶段失败也置 error（即便 overall 可能后续才打 done）
    if (progress.phase === "done" && progress.ok === false) {
      wizardState = "error";
      lastError = progress.reason ?? `${progress.stage}_failed`;
    }
    set({
      calibrationCards: {
        ...get().calibrationCards,
        [alias]: {
          ...cur,
          lastProgress: progress,
          wizardState,
          lastError,
        },
      },
    });
  },

  openCalWizard: (alias) => {
    const cur = get().calibrationCards[alias] ?? emptyCalCard();
    set({
      calibrationCards: {
        ...get().calibrationCards,
        [alias]: { ...cur, wizardOpen: true },
      },
    });
  },

  closeCalWizard: (alias) => {
    const cur = get().calibrationCards[alias] ?? emptyCalCard();
    set({
      calibrationCards: {
        ...get().calibrationCards,
        [alias]: { ...cur, wizardOpen: false },
      },
    });
  },

  resetCalCardError: (alias) => {
    const cur = get().calibrationCards[alias] ?? emptyCalCard();
    set({
      calibrationCards: {
        ...get().calibrationCards,
        [alias]: { ...cur, wizardState: "idle", lastError: null },
      },
    });
  },
}));
