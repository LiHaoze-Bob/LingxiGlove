/**
 * ============================================================================
 *  灵犀手套 Web APP — WebSocket 协议层（v1）
 *  与 ESP32 LingxiGlove_Main 端 WS 服务对齐的帧定义。
 *
 *  设计要点：
 *   1. 所有帧统一 envelope: { v, kind, ts, payload }
 *      - v: 协议版本（向前兼容用）
 *      - kind: 帧类型（4 类业务帧 + 2 类控制帧）
 *      - ts: 服务端 / 客户端时间戳 ms
 *      - payload: 帧数据，类型由 kind 决定（联合类型，下面分别定义）
 *   2. 4 类业务帧（服务端 → 客户端）：
 *      - snapshot     高频遥测快照（50~100ms / 帧）
 *      - message      对话气泡（手势识别 / ASR 结果 / 系统提示）
 *      - mic_state    PTT 状态机变更（与固件 ptt_detector 对齐）
 *      - audio_chunk  录音 PCM 块（base64 编码 16-bit LE 单声道）
 *   3. 2 类控制帧（双向）：
 *      - hello        握手 + 协议版本协商 + 客户端能力
 *      - ping/pong    心跳保活（5s 客户端发 ping，服务端回 pong）
 *   4. 客户端 → 服务端的"模拟控制"通道：control（仅开发期）
 *
 *  与 useGloveSystem Hook 的关系：
 *   - WSFrame 进入 wsClient.dispatch() 后被解码 + 校验
 *   - 业务帧的 payload 字段就是 useGloveSystem 已有的 SystemSnapshot /
 *     ConversationMessage / MicState 类型，无需二次转换。
 * ============================================================================
 */

import type {
  ConversationMessage,
  MicState,
  SystemSnapshot,
} from './types';

/** 当前协议版本号；服务端 / 客户端不一致时 wsClient 会拒绝连接 */
export const WS_PROTO_VERSION = 1;

/** 默认心跳周期（ms） */
export const WS_HEARTBEAT_MS = 5000;

/** 默认重连策略（指数退避） */
export const WS_RECONNECT_BASE_MS = 1000;
export const WS_RECONNECT_MAX_MS = 15000;

// ─── 帧类型字面量 ─────────────────────────────────────────────────────────

export type WSFrameKind =
  | 'snapshot'
  | 'message'
  | 'mic_state'
  | 'audio_chunk'
  | 'tts_audio'
  | 'hello'
  | 'ping'
  | 'pong'
  | 'control';

// ─── 业务帧 payload 定义 ──────────────────────────────────────────────────

/** mic_state 帧 payload：与 ptt_detector PttDecision 对齐 */
export interface MicStatePayload {
  state: MicState;
  /** 实时电平 0~1 */
  level?: number;
  /** 当前录音时长 ms */
  recordingMs?: number;
  /** 状态变化时上报的 |a|-1g 瞬时值，用于前端 IMU 卡显示 */
  accelDelta?: number;
}

/** audio_chunk 帧 payload：录音 PCM 块 */
export interface AudioChunkPayload {
  /** 块序号（从 0 起，单次录音内自增） */
  seq: number;
  /** 采样率（Hz），与固件 MIC_SAMPLE_RATE_HZ 对齐 */
  sampleRate: 16000;
  /** 位深 */
  bitsPerSample: 16;
  /** 通道数 */
  channels: 1;
  /** PCM 数据（base64 编码 16-bit LE）；末尾块为空字符串 + final=true */
  pcmBase64: string;
  /** 是否为本次录音的最后一块 */
  final: boolean;
}

/**
 * tts_audio 帧 payload：演示模式下端侧把 TTS PCM 同步推给 APP 播放。
 * 结构与 AudioChunkPayload 同构 + 可选 text（仅首帧 seq=0 携带原文）。
 * sampleRate 由端侧决定（speak 通常 24000，离线兜底 16000），不固定。
 */
export interface TtsAudioPayload {
  /** 块序号（从 0 起，单句 TTS 内自增） */
  seq: number;
  /** 采样率（Hz），随端侧 TTS 路径变化（Qwen=24000 / 离线=16000） */
  sampleRate: number;
  /** 位深，固定 16 */
  bitsPerSample: 16;
  /** 通道数，固定 1 */
  channels: 1;
  /** PCM 数据（base64 编码 16-bit LE）；末尾块为空字符串 + final=true */
  pcmBase64: string;
  /** 是否为本次 TTS 的最后一块 */
  final: boolean;
  /** 仅首帧 seq=0 时携带：本句原文，便于 APP 显示字幕 */
  text?: string;
}

/** hello 帧 payload：握手时双方互发 */
export interface HelloPayload {
  v: number;
  /** 客户端 / 服务端标识，便于日志区分 */
  role: 'client' | 'server';
  /** 设备/前端能力（保留扩展） */
  caps?: {
    /** 是否支持二进制 audio_chunk（false 时服务端会用 base64） */
    binaryAudio?: boolean;
    /** 客户端声明：是否订阅演示模式的 tts_audio 流 */
    demoMode?: boolean;
  };
}

/** control 帧 payload：客户端发给服务端的模拟控制（开发期用） */
export interface ControlPayload {
  /** 触发动作类型 */
  action:
    | 'mock_start_recording'  // 模拟双击 → 进入 RECORDING
    | 'mock_stop_recording'   // 模拟握拳 → 进入 PROCESSING
    | 'reset';
}

// ─── 通用 envelope ────────────────────────────────────────────────────────

interface BaseFrame<K extends WSFrameKind, P> {
  v: typeof WS_PROTO_VERSION;
  kind: K;
  ts: number;
  payload: P;
}

export type SnapshotFrame    = BaseFrame<'snapshot', SystemSnapshot>;
export type MessageFrame     = BaseFrame<'message', ConversationMessage>;
export type MicStateFrame    = BaseFrame<'mic_state', MicStatePayload>;
export type AudioChunkFrame  = BaseFrame<'audio_chunk', AudioChunkPayload>;
export type TtsAudioFrame    = BaseFrame<'tts_audio', TtsAudioPayload>;
export type HelloFrame       = BaseFrame<'hello', HelloPayload>;
export type PingFrame        = BaseFrame<'ping', { t: number }>;
export type PongFrame        = BaseFrame<'pong', { t: number }>;
export type ControlFrame     = BaseFrame<'control', ControlPayload>;

/** 联合类型：解码 / 分发用 */
export type WSFrame =
  | SnapshotFrame
  | MessageFrame
  | MicStateFrame
  | AudioChunkFrame
  | TtsAudioFrame
  | HelloFrame
  | PingFrame
  | PongFrame
  | ControlFrame;

// ─── 解码 / 校验 ──────────────────────────────────────────────────────────

/**
 * 安全解析 WS 文本帧。失败返回 null（已打印警告）。
 *
 * 校验：
 *   - JSON 可解
 *   - 顶层结构包含 v / kind / payload
 *   - 协议版本与 WS_PROTO_VERSION 一致
 */
export function decodeFrame(raw: string): WSFrame | null {
  let obj: unknown;
  try {
    obj = JSON.parse(raw);
  } catch (err) {
    console.warn('[wsProto] JSON parse error:', err);
    return null;
  }
  if (!obj || typeof obj !== 'object') {
    console.warn('[wsProto] frame is not an object:', obj);
    return null;
  }
  const f = obj as Partial<WSFrame> & { v?: number; kind?: string };
  if (f.v !== WS_PROTO_VERSION) {
    console.warn(
      `[wsProto] proto version mismatch: got ${f.v}, expected ${WS_PROTO_VERSION}`,
    );
    return null;
  }
  if (typeof f.kind !== 'string' || !('payload' in f)) {
    console.warn('[wsProto] missing kind/payload:', obj);
    return null;
  }
  // 简单按 kind 名校验枚举；具体 payload 内部由消费方按 type 信任
  const validKinds: WSFrameKind[] = [
    'snapshot',
    'message',
    'mic_state',
    'audio_chunk',
    'tts_audio',
    'hello',
    'ping',
    'pong',
    'control',
  ];
  if (!validKinds.includes(f.kind as WSFrameKind)) {
    console.warn('[wsProto] unknown kind:', f.kind);
    return null;
  }
  return obj as WSFrame;
}

/** 构造 envelope（自动填 v / ts） */
export function makeFrame<F extends WSFrame>(
  kind: F['kind'],
  payload: F['payload'],
): F {
  return {
    v: WS_PROTO_VERSION,
    kind,
    ts: Date.now(),
    payload,
  } as F;
}
