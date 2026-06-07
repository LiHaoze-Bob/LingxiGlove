/**
 * ============================================================================
 *  灵犀手套 Web APP — 共享类型定义
 *  v2: 双向对话气泡 + 工程仪表板架构
 * ============================================================================
 */

// ─── 5 路 Flex 弯曲传感器 ───────────────────────────────────────────────────
export type FlexArray5 = [number, number, number, number, number];
export const FINGER_NAMES = ['拇', '食', '中', '无', '小'] as const;
export type FingerKey = (typeof FINGER_NAMES)[number];

/** Flex ADC 校准范围 (来自 project memory: 5V 供电时实测) */
export const FLEX_ADC_MIN = 1100;
export const FLEX_ADC_MAX = 2800;

/** 单根手指 flex 状态 (规范化 + 阈值判定) */
export interface FingerState {
  raw: number;          // 原始 ADC 值 1100~2800
  normalized: number;   // 0=伸直, 1=弯曲
  bent: boolean;        // 是否触发弯曲阈值
}

// ─── IMU 姿态 ──────────────────────────────────────────────────────────────
export interface ImuState {
  pitch: number;        // ±90°
  roll: number;         // ±180°
  /** 加速度模长 - 1g 的瞬时偏差，用于 tap 检测 */
  accelDelta?: number;
  /** 角速度模长 (°/s) */
  gyroMag?: number;
}

// ─── 麦克风 / PTT 状态机 ──────────────────────────────────────────────────
/**
 * MIC 状态机：
 *   IDLE        — 初始/手势识别中
 *   WAITING_TAP — 检测到 5 指张开，等待双击
 *   ARMED       — 第一次敲击已检测，等第二次
 *   RECORDING   — 双击成功，正在录音
 *   PROCESSING  — 录音结束，ASR 识别中
 */
export type MicState =
  | 'IDLE'
  | 'WAITING_TAP'
  | 'ARMED'
  | 'RECORDING'
  | 'PROCESSING';

export interface MicStatus {
  state: MicState;
  /** 实时音频电平 0~1 (PTT 期间) */
  level: number;
  /** 简易频谱 16 段 0~1 (PTT 期间) */
  spectrum: number[];
  /** 当前录音时长 ms */
  recordingMs: number;
}

// ─── 手势识别候选 ─────────────────────────────────────────────────────────
export interface GestureCandidate {
  text: string;
  confidence: number;       // 0~1
}

export interface GestureRecognitionState {
  /** Top-3 候选 (置信度降序) */
  candidates: GestureCandidate[];
  /** 数据来源 */
  source: 'left' | 'right' | 'both' | 'none';
  /** 时间戳 (ms) */
  timestamp: number;
}

// ─── 系统连接状态 ─────────────────────────────────────────────────────────
export type ConnectionStatus = 'connected' | 'disconnected' | 'reconnecting';

export interface SystemHealth {
  connectionStatus: ConnectionStatus;
  /** WiFi RSSI (dBm) */
  rssi: number;
  /** 电池电量 0~100 */
  battery: number;
  /** WS 上行延迟 ms */
  latencyMs: number;
  /** 已连接时长 (秒) */
  uptimeSec: number;
  /** 数据包速率 pkt/s */
  packetRate: number;
  /** ESP32 IP */
  ip: string;
}

// ─── 单只手的遥测 ─────────────────────────────────────────────────────────
export interface HandTelemetry {
  fingers: Record<FingerKey, FingerState>;
  imu: ImuState;
}

export type HandSide = 'left' | 'right';

// ─── 整体快照 (单帧推送) ──────────────────────────────────────────────────
/** 手套实时遥测快照 (50~100ms 一次推送，双手并行) */
export interface SystemSnapshot {
  system: SystemHealth;
  hands: Record<HandSide, HandTelemetry>;
  mic: MicStatus;
  gesture: GestureRecognitionState;
}

// ─── 对话气泡 ─────────────────────────────────────────────────────────────
/**
 * 对话消息：
 *  - 'sign'   = 听障使用人通过手套表达的手语 (来自 GestureRecognition)
 *  - 'speech' = 健听者通过手套麦克风讲话 → ASR 转写
 *  - 'system' = 系统提示 (录音开始、识别中、错误等)
 */
export type MessageRole = 'sign' | 'speech' | 'system';

export interface ConversationMessage {
  id: string;                  // 唯一 id
  role: MessageRole;
  text: string;
  /** 创建时间 ms */
  timestamp: number;
  /** 置信度 (sign/speech 适用) 0~1 */
  confidence?: number;
  /** ASR 服务返回耗时 ms (speech 适用) */
  latencyMs?: number;
  /** 该气泡是否处于"识别中"占位态 (打字机/省略号动画) */
  pending?: boolean;
}

// ─── Hook 返回值 ─────────────────────────────────────────────────────────
export interface UseGloveSystemReturn {
  snapshot: SystemSnapshot;
  conversation: ConversationMessage[];
  /** 触发录音（开发态由 mock 调用，真实模式由手套敲击触发） */
  startRecording: () => void;
  stopRecording: () => void;
  /** 清空对话 */
  clearConversation: () => void;
  /** 演示模式开关：开启时端侧把 TTS 流推到 APP 同播 */
  demoMode: boolean;
  /** 切换演示模式（异步：会触发 WS 重连让端侧重新 hello）；
   *  返回 Promise 以便调用方在 user gesture 内 await 完成 AudioContext.resume() */
  setDemoMode: (enabled: boolean) => Promise<void>;
}

// ─── 默认 / 空快照 ───────────────────────────────────────────────────────
export const EMPTY_SNAPSHOT: SystemSnapshot = {
  system: {
    connectionStatus: 'disconnected',
    rssi: -100,
    battery: 0,
    latencyMs: 0,
    uptimeSec: 0,
    packetRate: 0,
    ip: '0.0.0.0',
  },
  hands: {
    left: {
      fingers: {
        拇: { raw: 2800, normalized: 0, bent: false },
        食: { raw: 2800, normalized: 0, bent: false },
        中: { raw: 2800, normalized: 0, bent: false },
        无: { raw: 2800, normalized: 0, bent: false },
        小: { raw: 2800, normalized: 0, bent: false },
      },
      imu: { pitch: 0, roll: 0 },
    },
    right: {
      fingers: {
        拇: { raw: 2800, normalized: 0, bent: false },
        食: { raw: 2800, normalized: 0, bent: false },
        中: { raw: 2800, normalized: 0, bent: false },
        无: { raw: 2800, normalized: 0, bent: false },
        小: { raw: 2800, normalized: 0, bent: false },
      },
      imu: { pitch: 0, roll: 0 },
    },
  },
  mic: { state: 'IDLE', level: 0, spectrum: new Array(16).fill(0), recordingMs: 0 },
  gesture: { candidates: [], source: 'none', timestamp: 0 },
};
