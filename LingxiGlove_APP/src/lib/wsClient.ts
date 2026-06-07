/**
 * ============================================================================
 *  灵犀手套 Web APP — WebSocket 客户端
 *
 *  职责：
 *   1. 连接 ESP32 LingxiGlove_Main 的 WS 服务（NEXT_PUBLIC_WS_URL 配置）
 *   2. 收发 wsProto 定义的 WSFrame，含 hello 握手 + 心跳 + 自动重连
 *   3. 通过 onSnapshot / onMessage / onMicState / onAudioChunk 回调
 *      把数据流推给上层 Hook（与 mock 模式对外接口完全一致）
 *
 *  状态机：
 *      ┌────── connect() ──────┐
 *      ▼                        ▼
 *   IDLE ─────────► CONNECTING ─────► OPEN ─────► CLOSING ─────► CLOSED
 *                                       │
 *                                       └─► (RECONNECTING via backoff timer)
 *
 *  使用：
 *      const client = createWsClient({
 *        url, onSnapshot, onMessage, onMicState,
 *      });
 *      client.start();
 *      ...
 *      client.stop();   // 调用方在 React unmount 时务必 stop
 * ============================================================================
 */

import type { ConversationMessage, MicState, SystemSnapshot } from './types';
import {
  WS_HEARTBEAT_MS,
  WS_RECONNECT_BASE_MS,
  WS_RECONNECT_MAX_MS,
  WS_PROTO_VERSION,
  decodeFrame,
  makeFrame,
  type AudioChunkPayload,
  type HelloFrame,
  type MicStatePayload,
  type PingFrame,
  type TtsAudioPayload,
  type WSFrame,
} from './wsProto';

export type WsConnectionState =
  | 'idle'
  | 'connecting'
  | 'open'
  | 'closing'
  | 'closed'
  | 'reconnecting';

export interface WsClientOptions {
  /** WS URL，必须以 ws:// 或 wss:// 开头 */
  url: string;
  onSnapshot?: (snap: SystemSnapshot) => void;
  onMessage?: (msg: ConversationMessage) => void;
  onMicState?: (payload: MicStatePayload) => void;
  onAudioChunk?: (payload: AudioChunkPayload) => void;
  /** 演示模式：端侧 TTS PCM 推流（仅在 hello.caps.demoMode=true 时端侧才发） */
  onTtsAudio?: (payload: TtsAudioPayload) => void;
  /** 连接状态变更（供 UI 显示重连提示） */
  onConnectionChange?: (state: WsConnectionState) => void;
  /** 心跳延迟回报（ms），UI 可用作 latencyMs 兜底显示 */
  onLatency?: (ms: number) => void;
  /**
   * 演示模式开关：true 时 hello 帧带 caps.demoMode=true，端侧据此开始推 tts_audio。
   * 切换需要 stop+start 重连让端侧重新读 hello（外部 hook 用 useEffect 依赖触发即可）。
   */
  demoMode?: boolean;
}

export interface WsClient {
  start(): void;
  stop(): void;
  /** 主动发送 control 帧（开发期模拟 PTT） */
  sendControl(action: 'mock_start_recording' | 'mock_stop_recording' | 'reset'): void;
  /** 当前连接状态（同步查询） */
  getState(): WsConnectionState;
}

/**
 * 创建 WS 客户端实例。函数内部维护 socket 引用、重连计时器、心跳计时器。
 * 不使用单例：调用方自行管理生命周期（与 React useEffect 对齐）。
 */
export function createWsClient(opts: WsClientOptions): WsClient {
  let ws: WebSocket | null = null;
  let state: WsConnectionState = 'idle';
  let reconnectTimer: ReturnType<typeof setTimeout> | null = null;
  let heartbeatTimer: ReturnType<typeof setInterval> | null = null;
  let lastPingSentAt = 0;
  let reconnectAttempts = 0;
  let stopped = false;

  const setState = (next: WsConnectionState) => {
    if (state !== next) {
      state = next;
      opts.onConnectionChange?.(next);
    }
  };

  const clearReconnectTimer = () => {
    if (reconnectTimer) {
      clearTimeout(reconnectTimer);
      reconnectTimer = null;
    }
  };

  const stopHeartbeat = () => {
    if (heartbeatTimer) {
      clearInterval(heartbeatTimer);
      heartbeatTimer = null;
    }
  };

  /** 启动心跳：每 WS_HEARTBEAT_MS 发一次 ping */
  const startHeartbeat = () => {
    stopHeartbeat();
    heartbeatTimer = setInterval(() => {
      if (!ws || ws.readyState !== WebSocket.OPEN) return;
      lastPingSentAt = Date.now();
      const frame = makeFrame<PingFrame>('ping', { t: lastPingSentAt });
      try {
        ws.send(JSON.stringify(frame));
      } catch (err) {
        console.warn('[wsClient] ping send error:', err);
      }
    }, WS_HEARTBEAT_MS);
  };

  /** 业务帧分发 */
  const dispatch = (frame: WSFrame) => {
    switch (frame.kind) {
      case 'snapshot':
        opts.onSnapshot?.(frame.payload);
        break;
      case 'message':
        opts.onMessage?.(frame.payload);
        break;
      case 'mic_state':
        opts.onMicState?.(frame.payload);
        break;
      case 'audio_chunk':
        opts.onAudioChunk?.(frame.payload);
        break;
      case 'tts_audio':
        opts.onTtsAudio?.(frame.payload);
        break;
      case 'hello':
        if (frame.payload.v !== WS_PROTO_VERSION) {
          console.warn(
            `[wsClient] server proto v${frame.payload.v} != client v${WS_PROTO_VERSION}, may be incompatible`,
          );
        } else {
          console.log('[wsClient] server hello OK');
        }
        break;
      case 'pong': {
        const rtt = Date.now() - (frame.payload?.t ?? lastPingSentAt);
        opts.onLatency?.(rtt);
        break;
      }
      case 'ping':
      case 'control':
        // 服务端不应主动发 ping/control，忽略
        break;
    }
  };

  /** 计算下次重连退避（指数 + 上限） */
  const nextReconnectDelay = () => {
    const exp = Math.min(
      WS_RECONNECT_MAX_MS,
      WS_RECONNECT_BASE_MS * Math.pow(2, reconnectAttempts),
    );
    return exp;
  };

  const scheduleReconnect = () => {
    if (stopped) return;
    setState('reconnecting');
    const delay = nextReconnectDelay();
    reconnectAttempts += 1;
    console.log(`[wsClient] reconnect in ${delay}ms (attempt ${reconnectAttempts})`);
    reconnectTimer = setTimeout(() => {
      reconnectTimer = null;
      connect();
    }, delay);
  };

  const connect = () => {
    if (stopped) return;
    setState('connecting');
    try {
      ws = new WebSocket(opts.url);
    } catch (err) {
      console.warn('[wsClient] WebSocket ctor error:', err);
      scheduleReconnect();
      return;
    }

    ws.onopen = () => {
      console.log('[wsClient] open');
      setState('open');
      reconnectAttempts = 0;
      // 发 hello 握手
      const hello = makeFrame<HelloFrame>('hello', {
        v: WS_PROTO_VERSION,
        role: 'client',
        caps: {
          binaryAudio: false,
          // 仅 demoMode=true 时显式带，避免污染默认 caps
          ...(opts.demoMode ? { demoMode: true } : {}),
        },
      });
      try {
        ws?.send(JSON.stringify(hello));
      } catch (err) {
        console.warn('[wsClient] hello send error:', err);
      }
      startHeartbeat();
    };

    ws.onmessage = (ev) => {
      // 文本帧：JSON envelope；二进制帧：未来音频专用通道（暂未实装）
      if (typeof ev.data !== 'string') {
        console.warn('[wsClient] binary frame received but not handled yet');
        return;
      }
      const frame = decodeFrame(ev.data);
      if (frame) dispatch(frame);
    };

    ws.onerror = (ev) => {
      console.warn('[wsClient] error:', ev);
    };

    ws.onclose = (ev) => {
      console.log(`[wsClient] close code=${ev.code} reason="${ev.reason}"`);
      stopHeartbeat();
      setState('closed');
      ws = null;
      if (!stopped) scheduleReconnect();
    };
  };

  return {
    start() {
      stopped = false;
      reconnectAttempts = 0;
      connect();
    },
    stop() {
      stopped = true;
      clearReconnectTimer();
      stopHeartbeat();
      if (ws) {
        setState('closing');
        try {
          ws.close(1000, 'client_stop');
        } catch (err) {
          console.warn('[wsClient] close error:', err);
        }
        ws = null;
      }
      setState('idle');
    },
    sendControl(action) {
      if (!ws || ws.readyState !== WebSocket.OPEN) {
        console.warn('[wsClient] sendControl skipped, ws not open');
        return;
      }
      const frame = makeFrame('control', { action });
      try {
        ws.send(JSON.stringify(frame));
      } catch (err) {
        console.warn('[wsClient] control send error:', err);
      }
    },
    getState() {
      return state;
    },
  };
}
