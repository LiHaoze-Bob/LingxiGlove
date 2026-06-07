/**
 * Tauri ↔ TS 共享类型
 * 与 src-tauri/src/types.rs 保持一一对应
 */

export type DeviceStatus = "idle" | "connected" | "error";

export interface SerialPortInfo {
  port_name: string;
  description: string | null;
  port_type: string; // USB / Bluetooth / PCI / Unknown
}

export interface DeviceMeta {
  alias: string;
  port: string;
  baud: number;
  status: DeviceStatus;
  frame_count: number;
}

export interface Frame {
  dev_alias: string;
  recv_ts_ms: number;
  dev_ts_ms: number;
  label: number;
  values: number[];
  raw_line: string;
}

export interface FpsSnapshot {
  alias: string;
  frame_count: number;
  fps: number;
}

/** Day 2 — 当前会话状态快照（get_session_info） */
export interface SessionInfo {
  recording: boolean;
  paused: boolean;
  session_id: string | null;
  rows_per_device: Record<string, number>;
}

/** Day 2 — 单设备结束摘要 */
export interface DeviceSessionSummary {
  alias: string;
  csv_path: string;
  rows: number;
  labeled_rows: number;
}

/** Day 2 — 整次会话结束摘要（stop_session 返回） */
export interface SessionSummary {
  session_id: string;
  started_at_ms: number;
  duration_ms: number;
  per_device: DeviceSessionSummary[];
}

/** 默认两个工位别名 + 推荐波特率 */
export const DEFAULT_ALIASES = ["left", "right"] as const;
export type DeviceAlias = (typeof DEFAULT_ALIASES)[number];

export const DEFAULT_BAUD = 115200;
export const COMMON_BAUDS = [9600, 38400, 57600, 115200, 230400, 460800, 921600];

/**
 * 端侧 firmware MODE_CAPTURE 的固定 schema
 * timestamp_ms,ax,ay,az,gx,gy,gz,pitch,roll,flex0..flex4,label
 *
 * frame.values 解析后 = 14 个 float（不含第一列 timestamp_ms）
 * 最后一个值是端侧 label，PC 写盘时会被覆写为 GUI label
 */
export const CHANNEL_NAMES = [
  "ax", "ay", "az",
  "gx", "gy", "gz",
  "pitch", "roll",
  "flex0", "flex1", "flex2", "flex3", "flex4",
  "label",
] as const;
export type ChannelName = (typeof CHANNEL_NAMES)[number];

/** 默认绘图通道（5 路 flex，是阶段 1 主路径） */
export const DEFAULT_PLOT_CHANNELS: ChannelName[] = [
  "flex0", "flex1", "flex2", "flex3", "flex4",
];

/**
 * 与端侧 firmware 角色检测对齐
 * Rust 端 `serial-role` 事件 payload
 */
export interface SerialRoleEvent {
  alias: string;
  /** "master" | "slave" | "unknown" */
  role: "master" | "slave" | "unknown";
}

/**
 * 与端侧 config.h `CAPTURE_LABEL_NAMES[]` 对齐
 * 调整时：① 端侧 config.h；② 这里；③ build_dataset.py LABEL_NAMES 三处必须一致
 *
 * **Day 5 起**：UI 上的 label 名走 store + localStorage（用户可双击改名）；
 * 这里的 LABEL_NAMES 仅作为 fallback。
 */
export const LABEL_NAMES: Record<number, string> = {
  [-1]: "unlabeled",
  0: "straight",
  1: "half",
  2: "full",
};

/**
 * Day 5 默认 label 表（10 类，演示词 + 团队介绍场景）
 * 用户可在 LabelEditor 双击重命名，持久化到 localStorage
 */
export const DEFAULT_LABEL_NAMES: string[] = [
  "你好",   // 0
  "谢谢",   // 1
  "我们",   // 2
  "灵犀",   // 3
  "智能",   // 4
  "手套",   // 5
  "团队",   // 6
  "来自",   // 7
  "浙江",   // 8
  "大学",   // 9
];

/** localStorage key for user-customized label names */
export const LABEL_STORAGE_KEY = "lingxi_capture_labels_v1";

// ---------------- Day 3 — 数据集切窗 + EI 上传 ----------------

/** Day 3 — list_sessions 返回的单条会话条目 */
export interface SessionEntry {
  session_id: string;
  path: string;
  raw_csv: string;
  rows: number;
  labeled_rows: number;
  /**
   * 各 label 出现次数，已按 label 升序排列（-1 在最前）。
   * 形如 `[[-1, 19], [0, 100], [1, 80]]`。
   * 前端按 `labelNames`（自定义） + `LABEL_NAMES`（fallback）翻译展示。
   */
  label_counts: Array<[number, number]>;
}

/** Day 3 — run_build_dataset / run_pipeline 入参 */
export interface BuildDatasetArgs {
  in_root: string;
  out_root: string;
  /** build_dataset.py 绝对路径（用户在设置中填） */
  script_path: string;
  /** Python 解释器（默认 python3） */
  python?: string;
  /** 透传给 build_dataset.py 的额外参数（如 --window 20） */
  extra_args?: string[];
  /** 可选：仅处理指定的 session_id。不传或为空数组 → 处理全部。 */
  session_ids?: string[];
}

/** Day 3 — 子进程退出摘要 */
export interface ProcessOutcome {
  ok: boolean;
  exit_code: number;
  stdout_lines: number;
  stderr_lines: number;
}

/** Day 3 — EI 上传摘要 */
export interface UploadOutcome {
  total: number;
  uploaded: number;
  failed: number;
}

/** Day 3 — 一键流水线摘要 */
export interface PipelineOutcome {
  build: ProcessOutcome;
  upload: UploadOutcome | null;
}

/** Day 3 — 进度事件 payload（与 Rust ProgressEvent 对齐） */
export interface PipelineProgress {
  stage: string; // build_dataset_stdout / build_dataset_stderr / build_dataset_done / upload_start / upload_progress / upload_done
  message: string;
  current: number;
  total: number;
  ok: boolean | null;
}

// ---------------- Day 6 — 校准 Tab ----------------

/**
 * 校准协议 — 与 src-tauri/src/calibration.rs 严格对齐
 *
 * Firmware 输出两类 marker：
 *   [CAL] stage=... phase=... [remain=N] [ok=0|1] [reason=...] [flags=N]
 *   [CAL_INFO] flags=N ax=... ay=... az=... gx=... gy=... gz=... [fmin=...] [fmax=...]
 *
 * Rust 端解析后通过 'calibration-event' 事件 emit 到前端。
 */
export type CalStage = "overall" | "imu" | "flex_min" | "flex_max" | "save";
export type CalPhase = "start" | "countdown" | "sampling" | "done";

export interface CalProgress {
  stage: CalStage;
  phase: CalPhase;
  remain: number | null;
  ok: boolean | null;
  reason: string | null;
  flags: number | null;
}

export interface CalInfo {
  /** bit0=IMU_VALID, bit1=FLEX_VALID */
  flags: number;
  accel_bias: [number, number, number];
  gyro_bias: [number, number, number];
  flex_min: [number, number, number, number, number];
  flex_max: [number, number, number, number, number];
}

/**
 * 设备配置信息（角色 / MAC / WiFi SSID）
 * 与 src-tauri/src/calibration.rs CfgInfo 对齐
 *
 * Firmware 输出: [CFG_INFO] role=master self_mac=AA:.. peer_mac=BB:..|none ssid=...
 */
export interface CfgInfo {
  /** "master" | "slave" */
  role: "master" | "slave";
  self_mac: string;
  /** null 表示 firmware 端打印了 peer_mac=none */
  peer_mac: string | null;
  ssid: string;
  /**
   * WiFi 密码（v4 [CFG_INFO] 新增）；空 / 未配置 / 老固件 = null。
   * 安全提示：固件以明文回传；UI 默认 password 类型隐藏，眼睛切换可见性。
   */
  wifi_pwd: string | null;
  /**
   * WiFi 是否已连接（WL_CONNECTED）。
   * 老固件不输出该字段时 Rust 端回退为 false。
   */
  wifi_connected: boolean;
  /** WiFi 已连接时的 IP；未连接 / 老固件 = null */
  ip: string | null;
  /** WiFi 已连接时的 RSSI（dBm，可为负）；未连接 / 老固件 = null */
  rssi: number | null;
  /**
   * 当前固件运行模式（v3 字段；老固件 = null）。
   *
   * 取值：
   *   - "recognize"       识别 + TTS（正常）
   *   - "capture"         词级数据采集（CSV 流）
   *   - "finger_spelling" 指拼采集
   *   - "accuracy_test"   准确率测试
   *   - "unknown"         固件未知模式
   *
   * 校准 Tab 据此显示模式徽章；非 recognize 时提供「恢复识别」一键复位。
   */
  mode: string | null;
}

/** Tauri 'calibration-event' payload（与 Rust CalEvent 对齐） */
export type CalEvent =
  | ({ kind: "progress"; alias: string } & CalProgress)
  | ({ kind: "info"; alias: string } & CalInfo)
  | ({ kind: "cfg"; alias: string } & CfgInfo);

/**
 * 校准 Tab 中两块板子各自的卡片状态
 *
 * 状态机：
 *   idle     — 未启动 / 已结束
 *   running  — wizard 已开启 + start_calibration 已发出，等待 marker 流
 *   error    — 收到 ok=0，等用户重试或关闭
 */
export type CalCardWizardState = "idle" | "running" | "error";

/** 校准 NVS bit 位掩码（与 firmware calibration.h 对齐） */
export const CAL_FLAG_IMU = 1;
export const CAL_FLAG_FLEX = 2;

/** 校准 Tab 的两个固定 alias（与 capture tab 的 left/right 隔离） */
export const CAL_LEFT_ALIAS = "CAL_LEFT";
export const CAL_RIGHT_ALIAS = "CAL_RIGHT";
export const CAL_ALIASES = [CAL_LEFT_ALIAS, CAL_RIGHT_ALIAS] as const;
export type CalAlias = (typeof CAL_ALIASES)[number];

