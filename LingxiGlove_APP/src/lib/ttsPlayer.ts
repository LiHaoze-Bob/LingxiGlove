/**
 * ============================================================================
 *  演示模式 TTS 流播放器（Web Audio API）
 *
 *  端侧 LingxiGlove_Main 在 hello.caps.demoMode=true 时，会把 speak() 与
 *  PlayPcmInt16() 内部每次 i2s_write 的 PCM 块同步通过 WS 推给 APP。
 *  本模块按 seq 顺序排队，借助 AudioContext 自动重采样并播放。
 *
 *  设计要点：
 *   1. AudioContext 懒加载：浏览器要求 user gesture 才能创建/resume，
 *      因此 createTtsPlayer() 不立即 new，首次 enqueue 或显式 resume() 时才创建；
 *      调用方应在用户切换"演示模式"开关的 onClick 里同步调用 resume()。
 *   2. 调度模型：维护 nextStartTime，每帧 source.start(nextStartTime)，
 *      nextStartTime += buffer.duration，相邻帧无缝拼接。
 *      首帧从 currentTime + 50ms 起，给出抖动余量。
 *   3. seq 处理：
 *      - seq=0 视为新句，丢弃任何在调度但还没播完的旧句调度（重置 nextStartTime）
 *      - seq <= lastSeq 且 seq != 0 视为乱序晚到，丢弃
 *      - final=true 且 pcmBase64 为空 → 收尾帧，仅记录 lastSeq，不动调度
 *   4. 采样率：端侧可能 16000（离线兜底）/24000（Qwen-TTS）切换，
 *      createBuffer(1, len, srcRate) 让 AudioContext 自动重采样到设备 rate。
 * ============================================================================
 */

import type { TtsAudioPayload } from './wsProto';

export interface TtsPlayer {
  /** 入队一帧；非演示模式下也是无副作用安全调用 */
  enqueue(chunk: TtsAudioPayload): void;
  /** 用户手势触发：创建/恢复 AudioContext。必须在 click 等 user gesture 内调用 */
  resume(): Promise<void>;
  /** 立即丢弃当前调度（不影响已排队但未播完的 source 自然衰减） */
  reset(): void;
  /** 释放资源（unmount 时） */
  dispose(): void;
}

interface AudioContextCtor {
  new (options?: AudioContextOptions): AudioContext;
}

function getAudioContextCtor(): AudioContextCtor | null {
  if (typeof window === 'undefined') return null;
  // Safari 仍需 webkitAudioContext 前缀
  const w = window as unknown as {
    AudioContext?: AudioContextCtor;
    webkitAudioContext?: AudioContextCtor;
  };
  return w.AudioContext ?? w.webkitAudioContext ?? null;
}

function decodeBase64Int16(b64: string): Int16Array {
  if (!b64) return new Int16Array(0);
  const bin = atob(b64);
  const bytes = new Uint8Array(bin.length);
  for (let i = 0; i < bin.length; i++) bytes[i] = bin.charCodeAt(i);
  // 16-bit LE：浏览器默认小端，字节流直接当作 Int16Array 视图
  // 注意 byteLength 必须是 2 的倍数；端侧广播的块都是按 sample 对齐
  const aligned = bytes.byteLength - (bytes.byteLength % 2);
  return new Int16Array(bytes.buffer, bytes.byteOffset, aligned / 2);
}

export function createTtsPlayer(): TtsPlayer {
  let ctx: AudioContext | null = null;
  let nextStartTime = 0;
  let lastSeq = -1;

  const ensureCtx = (): AudioContext | null => {
    if (ctx) return ctx;
    const Ctor = getAudioContextCtor();
    if (!Ctor) {
      console.warn('[ttsPlayer] Web Audio API 不可用');
      return null;
    }
    try {
      ctx = new Ctor();
    } catch (err) {
      console.warn('[ttsPlayer] AudioContext 创建失败:', err);
      return null;
    }
    return ctx;
  };

  const enqueue = (chunk: TtsAudioPayload) => {
    const c = ensureCtx();
    if (!c) return;

    // seq=0 视为新句开始：清空调度
    if (chunk.seq === 0) {
      lastSeq = -1;
      nextStartTime = 0;
    } else if (chunk.seq <= lastSeq) {
      // 乱序晚到，丢弃
      console.warn(
        `[ttsPlayer] 丢弃乱序帧 seq=${chunk.seq} (lastSeq=${lastSeq})`,
      );
      return;
    }

    // 收尾空帧：不调度，仅记录
    if (!chunk.pcmBase64) {
      lastSeq = chunk.seq;
      return;
    }

    const i16 = decodeBase64Int16(chunk.pcmBase64);
    if (i16.length === 0) {
      lastSeq = chunk.seq;
      return;
    }

    // Int16 → Float32（[-1, 1)）
    const f32 = new Float32Array(i16.length);
    const inv = 1 / 32768;
    for (let i = 0; i < i16.length; i++) f32[i] = i16[i] * inv;

    let buf: AudioBuffer;
    try {
      buf = c.createBuffer(1, f32.length, chunk.sampleRate);
    } catch (err) {
      // 极少数浏览器对 sampleRate 范围有要求
      console.warn(
        `[ttsPlayer] createBuffer 失败 rate=${chunk.sampleRate}:`,
        err,
      );
      return;
    }
    buf.copyToChannel(f32, 0);

    const src = c.createBufferSource();
    src.buffer = buf;
    src.connect(c.destination);

    const now = c.currentTime;
    // 50ms 启动延迟，给浏览器调度抖动留余量
    if (nextStartTime < now + 0.02) {
      nextStartTime = now + 0.05;
    }
    try {
      src.start(nextStartTime);
    } catch (err) {
      console.warn('[ttsPlayer] source.start 失败:', err);
      return;
    }
    nextStartTime += buf.duration;
    lastSeq = chunk.seq;
  };

  return {
    enqueue,
    async resume() {
      const c = ensureCtx();
      if (!c) return;
      if (c.state === 'suspended') {
        try {
          await c.resume();
        } catch (err) {
          console.warn('[ttsPlayer] resume 失败:', err);
        }
      }
    },
    reset() {
      lastSeq = -1;
      nextStartTime = 0;
    },
    dispose() {
      if (ctx) {
        ctx.close().catch(() => undefined);
        ctx = null;
      }
      lastSeq = -1;
      nextStartTime = 0;
    },
  };
}
