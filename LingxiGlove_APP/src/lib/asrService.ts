/**
 * ============================================================================
 *  灵犀手套 Web APP — ASR 服务模块
 *
 *  职责：
 *   1. AsrSession: 缓冲 WebSocket 接收到的 audio_chunk PCM 块
 *   2. submitAsr(): 将完整 PCM 数据发送到 /api/asr，返回识别文本
 *
 *  与 useGloveSystem 的关系：
 *   - Hook 收到 audio_chunk 帧时调用 session.appendChunk()
 *   - 收到 final=true 时调用 session.finalize() + submitAsr()
 * ============================================================================
 */

import type { AudioChunkPayload } from './wsProto';

// ─── 类型定义 ───────────────────────────────────────────────────────────────

export interface AsrResult {
  /** 识别文本 */
  text: string;
  /** ASR 服务端耗时 ms */
  latencyMs: number;
  /** 阿里云 task_id（用于排障） */
  taskId: string;
}

// ─── PCM 缓冲 Session ──────────────────────────────────────────────────────

/**
 * 管理单次录音的 PCM 数据缓冲。
 * 每次 PTT 录音周期创建/复用一个 session：
 *   appendChunk → appendChunk → ... → finalize → reset
 */
export class AsrSession {
  private chunks: Uint8Array[] = [];
  private totalBytes = 0;

  /** 追加一块 audio_chunk PCM 数据（base64 解码） */
  appendChunk(payload: AudioChunkPayload): void {
    if (!payload.pcmBase64 || payload.pcmBase64.length === 0) return;
    const binary = atob(payload.pcmBase64);
    const bytes = new Uint8Array(binary.length);
    for (let i = 0; i < binary.length; i++) {
      bytes[i] = binary.charCodeAt(i);
    }
    this.chunks.push(bytes);
    this.totalBytes += bytes.length;
  }

  /** 合并所有 chunk 为完整 PCM Uint8Array */
  finalize(): Uint8Array {
    if (this.chunks.length === 0) return new Uint8Array(0);
    if (this.chunks.length === 1) return this.chunks[0];
    const result = new Uint8Array(this.totalBytes);
    let offset = 0;
    for (const chunk of this.chunks) {
      result.set(chunk, offset);
      offset += chunk.length;
    }
    return result;
  }

  /** 重置缓冲，准备下一次录音 */
  reset(): void {
    this.chunks = [];
    this.totalBytes = 0;
  }

  /** 当前已缓冲的字节数 */
  get byteLength(): number {
    return this.totalBytes;
  }

  /** 当前已缓冲的 chunk 数量 */
  get chunkCount(): number {
    return this.chunks.length;
  }
}

// ─── ASR 提交 ───────────────────────────────────────────────────────────────

/**
 * 将完整 PCM 数据提交到 /api/asr 代理，获取识别结果。
 * 失败时抛出 Error（上层 catch 处理）。
 */
export async function submitAsr(pcm: Uint8Array): Promise<AsrResult> {
  if (pcm.length === 0) {
    throw new Error('PCM 数据为空，无法识别');
  }

  const response = await fetch('/api/asr', {
    method: 'POST',
    headers: {
      'Content-Type': 'application/octet-stream',
    },
    body: pcm as unknown as BodyInit,
  });

  const data = await response.json();

  if (!response.ok) {
    throw new Error(data.error || `ASR 请求失败 (${response.status})`);
  }

  return {
    text: data.text,
    latencyMs: data.latencyMs,
    taskId: data.taskId,
  };
}
