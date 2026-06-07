'use client';

import { useCallback, useEffect, useRef, useState } from 'react';
import {
  ConversationMessage,
  EMPTY_SNAPSHOT,
  MicState,
  SystemSnapshot,
  UseGloveSystemReturn,
} from '@/lib/types';
import { startMockStream } from '@/mock/mockServer';
import { createWsClient, type WsClient, type WsConnectionState } from '@/lib/wsClient';
import type { AudioChunkPayload, MicStatePayload, TtsAudioPayload } from '@/lib/wsProto';
import { createTtsPlayer, type TtsPlayer } from '@/lib/ttsPlayer';
import { AsrSession, submitAsr } from '@/lib/asrService';

const MAX_CONVERSATION = 80;

// 演示模式持久化键：刷新页面后保留用户的开关状态
const DEMO_MODE_STORAGE_KEY = 'lingxi.demoMode';

// Lv3a 滚动切片：APP 端硬保护，防止单段超过阿里云一句话识别 60s 上限。
// 50 秒 × 16 kHz × 2 byte = 1,600,000 byte ≈ 1.53 MiB；到点先 finalize 一段
// 提交识别，然后立即 reset 继续接收下一段，全部以"多段气泡"形式呈现。
//
// 与端侧 WS_MIC_STREAM_MAX_MS=55000ms 配合（端侧 55s 自动 mic off → 末段 final=true）：
//  - 用户正常 mic off 且 < 50s：单段，气泡无段号前缀
//  - 用户超过 50s 或忘按 mic off：触发本切片，多段提交，气泡显示 [1] / [2] / ...
const ASR_SLICE_BYTES = 50 * 16000 * 2;

/**
 * 灵犀手套系统 Hook (v2)。
 *
 * 提供：
 *  - snapshot:     高频遥测 (50ms 一次)
 *  - conversation: 双向对话气泡列表 (sign + speech + system)
 *
 * 数据源：
 *  - 默认 Mock：本地随机生成，方便 UI 调试
 *  - 真实 WebSocket：通过 NEXT_PUBLIC_WS_URL 配置 ws://192.168.1.100:81/ws，
 *    走 wsClient（带 hello 握手 + 5s 心跳 + 指数退避自动重连）。
 *
 * 真实模式下 mic_state 帧会同步驱动 snapshot.mic.state，并在状态翻转时
 * 注入对应的 system 气泡（与 mock 模式行为一致）。
 */
export function useGloveSystem(): UseGloveSystemReturn {
  const [snapshot, setSnapshot] = useState<SystemSnapshot>(EMPTY_SNAPSHOT);
  const [conversation, setConversation] = useState<ConversationMessage[]>([]);
  // 演示模式：从 localStorage 恢复，触发时通过 setDemoMode 更新（仅在客户端读取避免 SSR 不一致）
  const [demoMode, setDemoModeState] = useState(false);
  const wsClientRef = useRef<WsClient | null>(null);
  const ttsPlayerRef = useRef<TtsPlayer | null>(null);
  const asrSessionRef = useRef(new AsrSession());
  // 滚动切片：本次录音内已提交的段计数；final=true 后归零
  const segmentSeqRef = useRef(0);
  // 上一次已注入气泡的手势识别 timestamp；防止 snapshot 重复触发同一条手势气泡
  const lastGestureTsRef = useRef(0);

  // 首次挂载时读 localStorage
  useEffect(() => {
    if (typeof window === 'undefined') return;
    try {
      const raw = window.localStorage.getItem(DEMO_MODE_STORAGE_KEY);
      if (raw === '1') setDemoModeState(true);
    } catch {
      // 忽略 localStorage 不可用（隐私模式 / SSR）
    }
  }, []);

  const pushMessage = useCallback((msg: ConversationMessage) => {
    setConversation((prev) => {
      // 同 id 的消息视为对原占位（pending）的更新：原地替换，保持时序与位置
      const idx = prev.findIndex((m) => m.id === msg.id);
      if (idx >= 0) {
        const next = prev.slice();
        next[idx] = msg;
        return next;
      }
      const next = [...prev, msg];
      return next.length > MAX_CONVERSATION ? next.slice(-MAX_CONVERSATION) : next;
    });
  }, []);

  // mic_state 翻转 → 注入 system 气泡（mock 与 real 共用）
  const handleMicState = useCallback(
    (state: MicState) => {
      if (state === 'WAITING_TAP') {
        pushMessage({
          id: 'sys-' + Date.now(),
          role: 'system',
          text: '检测到双击，准备录音…',
          timestamp: Date.now(),
        });
      }
    },
    [pushMessage],
  );

  // 真实模式：mic_state payload → 同步进 snapshot.mic + 触发 handleMicState
  const handleRealMicState = useCallback(
    (payload: MicStatePayload) => {
      setSnapshot((prev) => ({
        ...prev,
        mic: {
          ...prev.mic,
          state: payload.state,
          level: payload.level ?? prev.mic.level,
          recordingMs: payload.recordingMs ?? prev.mic.recordingMs,
        },
      }));
      handleMicState(payload.state);
    },
    [handleMicState],
  );

  // ASR 链路：缓冲 audio_chunk，final 时提交识别；
  // 单段录音超过 ASR_SLICE_BYTES（~50s）时 Lv3a 滚动切片，提前提交并继续接收。
  const handleAudioChunk = useCallback(
    (chunk: AudioChunkPayload) => {
      const session = asrSessionRef.current;
      session.appendChunk(chunk);

      const isFinal = chunk.final;
      const overSize = !isFinal && session.byteLength >= ASR_SLICE_BYTES;
      if (!isFinal && !overSize) return;

      const pcm = session.finalize();
      session.reset();

      if (pcm.length === 0) {
        if (isFinal) segmentSeqRef.current = 0;
        return;
      }

      // 多段判定：要么本次是 overSize 切片，要么前面已经发过切片（本次是末段）
      const isMultiSegment = overSize || segmentSeqRef.current >= 1;
      segmentSeqRef.current += 1;
      const segIdx = isMultiSegment ? segmentSeqRef.current : 0;
      const prefix = segIdx > 0 ? `[${segIdx}] ` : '';

      const pendingId = 'speech-' + Date.now() + (segIdx > 0 ? '-s' + segIdx : '');
      pushMessage({
        id: pendingId,
        role: 'speech',
        text: prefix + '识别中…',
        timestamp: Date.now(),
        pending: true,
      });

      submitAsr(pcm)
        .then((result) => {
          pushMessage({
            id: pendingId,
            role: 'speech',
            text: prefix + result.text,
            confidence: 0.95,
            latencyMs: result.latencyMs,
            timestamp: Date.now(),
          });
        })
        .catch((err) => {
          pushMessage({
            id: pendingId,
            role: 'system',
            text: `ASR 失败${prefix ? ' ' + prefix.trim() : ''}: ${
              err instanceof Error ? err.message : String(err)
            }`,
            timestamp: Date.now(),
          });
        });

      // 录音整体结束 → 段计数归零，下次录音从 0 开始
      if (isFinal) segmentSeqRef.current = 0;
    },
    [pushMessage],
  );

  // 真实模式：连接状态变化 → 同步 snapshot.system.connectionStatus
  const handleConnectionChange = useCallback((wsState: WsConnectionState) => {
    setSnapshot((prev) => {
      const mapped =
        wsState === 'open'
          ? 'connected'
          : wsState === 'reconnecting' || wsState === 'connecting'
            ? 'reconnecting'
            : 'disconnected';
      if (prev.system.connectionStatus === mapped) return prev;
      return {
        ...prev,
        system: { ...prev.system, connectionStatus: mapped },
      };
    });
  }, []);

  // 真实模式：心跳 RTT → 同步 snapshot.system.latencyMs
  const handleLatency = useCallback((ms: number) => {
    setSnapshot((prev) => ({
      ...prev,
      system: { ...prev.system, latencyMs: ms },
    }));
  }, []);

  // 演示模式：tts_audio 帧 → ttsPlayer 队列
  //   首帧 text 不再单独注入字幕气泡：手势识别气泡（sign 角色）已在 handleSnapshot
  //   中按 gesture.timestamp 跳变注入，二者语义重叠，避免视觉重复。
  const handleTtsAudio = useCallback((payload: TtsAudioPayload) => {
    const player = ttsPlayerRef.current;
    if (!player) return;
    player.enqueue(payload);
  }, []);

  // 真实模式：snapshot 帧 merge（保留 APP 自己维护的 connectionStatus / latencyMs）
  // 端侧 snapshot.system 中这两个字段没有事实根据：connectionStatus 由 WS open/close
  // 状态计算，latencyMs 由 ping/pong 测算，端侧硬写会覆盖真值。
  //
  // 同时：snapshot.gesture.timestamp 跳变 → 注入一条 sign 气泡作为手语对话记录。
  // 端侧仲裁器播报手势后会刷新 g_lastGesture.timestamp_ms，APP 据此判定"有新识别"。
  const handleSnapshot = useCallback(
    (next: SystemSnapshot) => {
      setSnapshot((prev) => ({
        ...next,
        system: {
          ...next.system,
          connectionStatus: prev.system.connectionStatus,
          latencyMs: prev.system.latencyMs,
        },
      }));

      const g = next.gesture;
      const top = g.candidates[0];
      if (top && g.timestamp && g.timestamp !== lastGestureTsRef.current) {
        lastGestureTsRef.current = g.timestamp;
        pushMessage({
          id: 'gst-' + g.timestamp,
          role: 'sign',
          text: top.text,
          confidence: top.confidence,
          timestamp: Date.now(),
        });
      }
    },
    [pushMessage],
  );

  useEffect(() => {
    const wsUrl = process.env.NEXT_PUBLIC_WS_URL;
    const isMock = !wsUrl || wsUrl === 'mock';

    if (isMock) {
      console.log('[GloveSystem] Mock mode enabled');
      const cleanup = startMockStream({
        onSnapshot: setSnapshot,
        onMessage: pushMessage,
        onMicState: handleMicState,
        onAudioChunk: handleAudioChunk,
      });
      return cleanup;
    }

    console.log(`[GloveSystem] Connecting to ${wsUrl} (demoMode=${demoMode})`);
    // 演示模式开启时懒加载 TTS 播放器（AudioContext 仍在用户手势 resume 时才真正激活）
    if (demoMode && !ttsPlayerRef.current) {
      ttsPlayerRef.current = createTtsPlayer();
    }
    const client = createWsClient({
      url: wsUrl,
      onSnapshot: handleSnapshot,
      onMessage: pushMessage,
      onMicState: handleRealMicState,
      onAudioChunk: handleAudioChunk,
      onTtsAudio: demoMode ? handleTtsAudio : undefined,
      onConnectionChange: handleConnectionChange,
      onLatency: handleLatency,
      demoMode,
    });
    wsClientRef.current = client;
    client.start();

    return () => {
      client.stop();
      wsClientRef.current = null;
    };
  }, [
    pushMessage,
    handleMicState,
    handleRealMicState,
    handleAudioChunk,
    handleConnectionChange,
    handleLatency,
    handleSnapshot,
    handleTtsAudio,
    demoMode,
  ]);

  const startRecording = useCallback(() => {
    // 真实模式：发 control 帧让固件模拟双击；mock 模式下直接注入 system 气泡
    const client = wsClientRef.current;
    if (client) {
      client.sendControl('mock_start_recording');
      return;
    }
    pushMessage({
      id: 'sys-' + Date.now(),
      role: 'system',
      text: '手动开始录音',
      timestamp: Date.now(),
    });
  }, [pushMessage]);

  const stopRecording = useCallback(() => {
    const client = wsClientRef.current;
    if (client) {
      client.sendControl('mock_stop_recording');
    }
  }, []);

  const clearConversation = useCallback(() => setConversation([]), []);

  // 切换演示模式：写入 localStorage + 触发 useEffect 重建 wsClient（端侧重读 hello.caps）
  // 返回 Promise 以便 UI 在 user gesture 内 await，确保 AudioContext.resume() 一并完成
  const setDemoMode = useCallback(async (enabled: boolean) => {
    if (typeof window !== 'undefined') {
      try {
        window.localStorage.setItem(DEMO_MODE_STORAGE_KEY, enabled ? '1' : '0');
      } catch {
        // 忽略 localStorage 不可用
      }
    }
    if (enabled) {
      // 提前创建并 resume；createTtsPlayer 是幂等的（保留旧实例）
      if (!ttsPlayerRef.current) {
        ttsPlayerRef.current = createTtsPlayer();
      }
      await ttsPlayerRef.current.resume();
    } else {
      // 关闭时立即停止当前调度但保留实例（下次开启免重建）
      ttsPlayerRef.current?.reset();
    }
    setDemoModeState(enabled);
  }, []);

  // 卸载时释放 ttsPlayer
  useEffect(() => {
    return () => {
      ttsPlayerRef.current?.dispose();
      ttsPlayerRef.current = null;
    };
  }, []);

  return {
    snapshot,
    conversation,
    startRecording,
    stopRecording,
    clearConversation,
    demoMode,
    setDemoMode,
  };
}
