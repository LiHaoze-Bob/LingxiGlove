/**
 * ============================================================================
 *  Mock 数据流 — v2 双向对话 + 工程仪表板
 *  - 50ms 一帧推送 SystemSnapshot (高频遥测)
 *  - 随机生成手语识别消息 (3~6s 间隔)
 *  - 随机模拟健听者讲话 ASR 消息 (5~10s 间隔)
 * ============================================================================
 */
import {
  SystemSnapshot,
  ConversationMessage,
  FlexArray5,
  FINGER_NAMES,
  FLEX_ADC_MAX,
  FLEX_ADC_MIN,
  EMPTY_SNAPSHOT,
  MicState,
  FingerKey,
  HandSide,
  HandTelemetry,
} from '@/lib/types';

// ─── 词库 ─────────────────────────────────────────────────────────────────
const SIGN_PHRASES = [
  '你好',
  '谢谢',
  '再见',
  '我爱你',
  '加油',
  '帮助',
  '一起',
  '是的',
  '不是',
  '我',
];

const SPEECH_PHRASES = [
  '你好，请问需要什么帮助？',
  '收到，我已经记下来了。',
  '您说得很清楚，谢谢。',
  '我们去那边坐下聊吧。',
  '没关系，慢慢来。',
  '好的，我明白了。',
  '今天天气真不错。',
  '请问咖啡还是茶？',
  '稍等一下，我马上回来。',
  '今天医院的检查结果出来了，医生说目前一切都很正常，不过他建议我们最近还是要多注意休息，少熬夜，每天保证至少八小时睡眠，饮食上也要尽量清淡一些，多吃蔬菜水果，少吃油腻和辛辣的食物，每周至少抽时间出来散步两三次。',
];

// ─── 工具函数 ─────────────────────────────────────────────────────────────
function randomBetween(min: number, max: number): number {
  return min + Math.random() * (max - min);
}

function randomFlexRaw(bent: boolean): number {
  if (bent) return Math.floor(randomBetween(FLEX_ADC_MIN, FLEX_ADC_MIN + 400));
  return Math.floor(randomBetween(FLEX_ADC_MAX - 400, FLEX_ADC_MAX));
}

function makeId(): string {
  return Date.now().toString(36) + Math.random().toString(36).slice(2, 8);
}

// ─── 流控状态 ─────────────────────────────────────────────────────────────
interface MockRuntime {
  uptimeSec: number;
  micState: MicState;
  micStateStartedAt: number;
  /** 当前正在 mock 的手语消息（为 null 表示待生成下一条） */
  nextSignAt: number;
  nextSpeechAt: number;
  /** speech 轮次计数，用于在首轮挑选最长文本展示折叠效果 */
  speechCycleCount: number;
  /** 左右手独立的 bent 模板 */
  bentTemplate: Record<HandSide, Record<FingerKey, boolean>>;
  bentUntil: Record<HandSide, number>;
}

function freshBentTemplate(): Record<FingerKey, boolean> {
  const tpl: Record<FingerKey, boolean> = {} as Record<FingerKey, boolean>;
  FINGER_NAMES.forEach((f) => (tpl[f] = Math.random() < 0.45));
  return tpl;
}

function buildHandFingers(
  template: Record<FingerKey, boolean>,
): HandTelemetry['fingers'] {
  return Object.fromEntries(
    FINGER_NAMES.map((f) => {
      const bent = template[f];
      const raw = randomFlexRaw(bent);
      const normalized =
        1 - (raw - FLEX_ADC_MIN) / (FLEX_ADC_MAX - FLEX_ADC_MIN);
      return [
        f,
        { raw, normalized: Math.max(0, Math.min(1, normalized)), bent },
      ];
    }),
  ) as HandTelemetry['fingers'];
}

function buildHandImu(
  t: number,
  phase: number,
): HandTelemetry['imu'] {
  return {
    pitch: 25 * Math.sin(t * 0.7 + phase) + randomBetween(-3, 3),
    roll: 40 * Math.sin(t * 0.4 + phase) + randomBetween(-5, 5),
    accelDelta: Math.abs(randomBetween(-0.3, 0.3)),
    gyroMag: Math.abs(randomBetween(0, 80)),
  };
}

// ─── 主流 ─────────────────────────────────────────────────────────────────
export interface MockHandlers {
  onSnapshot: (snap: SystemSnapshot) => void;
  onMessage: (msg: ConversationMessage) => void;
  onMicState: (state: MicState) => void;
  onAudioChunk?: (payload: import('@/lib/wsProto').AudioChunkPayload) => void;
}

/**
 * 启动模拟数据流。
 * @returns cleanup 函数
 */
export function startMockStream(handlers: MockHandlers): () => void {
  const startMs = Date.now();
  const rt: MockRuntime = {
    uptimeSec: 0,
    micState: 'IDLE',
    micStateStartedAt: startMs,
    nextSignAt: startMs + 2000,
    nextSpeechAt: startMs + 6000,
    speechCycleCount: 0,
    bentTemplate: {
      left: freshBentTemplate(),
      right: freshBentTemplate(),
    },
    bentUntil: {
      left: startMs + 1500,
      right: startMs + 1800,
    },
  };

  // 高频遥测 (50ms = 20Hz)
  const telemetryTimer = setInterval(() => {
    const now = Date.now();
    rt.uptimeSec = Math.floor((now - startMs) / 1000);

    // 左右手 bent 模板各自独立切换（模拟手势左右不同步）
    (['left', 'right'] as const).forEach((side) => {
      if (now > rt.bentUntil[side]) {
        rt.bentTemplate[side] = freshBentTemplate();
        rt.bentUntil[side] = now + randomBetween(1200, 2500);
      }
    });

    const t = (now - startMs) / 1000;
    const hands: SystemSnapshot['hands'] = {
      left: {
        fingers: buildHandFingers(rt.bentTemplate.left),
        imu: buildHandImu(t, 0),
      },
      right: {
        fingers: buildHandFingers(rt.bentTemplate.right),
        imu: buildHandImu(t, 1.7),
      },
    };

    // MIC 状态机推进
    advanceMicState(rt, now, handlers);
    const mic = buildMicStatus(rt, now);

    // 系统健康
    const system = {
      connectionStatus: 'connected' as const,
      rssi: Math.floor(-58 + Math.sin(t * 0.3) * 6 + randomBetween(-2, 2)),
      battery: Math.max(0, Math.min(100, 87 - rt.uptimeSec / 600)),
      latencyMs: Math.floor(randomBetween(8, 28)),
      uptimeSec: rt.uptimeSec,
      packetRate: 20,
      ip: '192.168.1.100',
    };

    // 手势识别 Top-3 (基于当前弯曲模板模拟)
    const gestureBase = SIGN_PHRASES[(rt.uptimeSec >> 1) % SIGN_PHRASES.length];
    const candidates = [
      { text: gestureBase, confidence: 0.78 + Math.random() * 0.18 },
      {
        text: SIGN_PHRASES[(rt.uptimeSec + 3) % SIGN_PHRASES.length],
        confidence: 0.08 + Math.random() * 0.1,
      },
      {
        text: SIGN_PHRASES[(rt.uptimeSec + 5) % SIGN_PHRASES.length],
        confidence: 0.02 + Math.random() * 0.05,
      },
    ];

    const snap: SystemSnapshot = {
      system,
      hands,
      mic,
      gesture: {
        candidates,
        source: Math.random() < 0.6 ? 'right' : 'both',
        timestamp: now,
      },
    };
    handlers.onSnapshot(snap);

    // 触发对话消息
    if (now >= rt.nextSignAt && rt.micState === 'IDLE') {
      const text = SIGN_PHRASES[Math.floor(Math.random() * SIGN_PHRASES.length)];
      handlers.onMessage({
        id: makeId(),
        role: 'sign',
        text,
        timestamp: now,
        confidence: 0.78 + Math.random() * 0.2,
      });
      rt.nextSignAt = now + randomBetween(3500, 7000);
    }

    if (now >= rt.nextSpeechAt) {
      // 模拟整个 PTT 周期：WAITING_TAP -> ARMED -> RECORDING -> PROCESSING -> 文本
      triggerSpeechCycle(rt, now, handlers);
      rt.nextSpeechAt = now + randomBetween(8000, 14000);
    }
  }, 50);

  // 立刻发一次空快照让 UI 上线
  handlers.onSnapshot({ ...EMPTY_SNAPSHOT, system: {
    ...EMPTY_SNAPSHOT.system,
    connectionStatus: 'connected',
    ip: '192.168.1.100',
    rssi: -58,
    battery: 87,
  } });

  return () => clearInterval(telemetryTimer);
}

// ─── MIC 状态机推进 ───────────────────────────────────────────────────────
function advanceMicState(rt: MockRuntime, now: number, handlers: MockHandlers) {
  const elapsed = now - rt.micStateStartedAt;
  let next: MicState | null = null;
  switch (rt.micState) {
    case 'WAITING_TAP':
      if (elapsed > 700) next = 'ARMED';
      break;
    case 'ARMED':
      if (elapsed > 400) next = 'RECORDING';
      break;
    case 'RECORDING':
      if (elapsed > 2200) next = 'PROCESSING';
      break;
    case 'PROCESSING':
      if (elapsed > 1800) next = 'IDLE';
      break;
    default:
      break;
  }
  if (next && next !== rt.micState) {
    rt.micState = next;
    rt.micStateStartedAt = now;
    handlers.onMicState(next);
  }
}

function buildMicStatus(rt: MockRuntime, now: number): SystemSnapshot['mic'] {
  const elapsed = now - rt.micStateStartedAt;
  const recording = rt.micState === 'RECORDING';
  const level = recording ? 0.4 + 0.5 * Math.abs(Math.sin(now * 0.012)) : 0;
  const spectrum = new Array(16).fill(0).map((_, i) => {
    if (!recording) return 0;
    const phase = now * 0.008 + i * 0.4;
    return Math.max(0.02, Math.abs(Math.sin(phase)) * (0.4 + Math.random() * 0.6));
  });
  return {
    state: rt.micState,
    level,
    spectrum,
    recordingMs: recording ? elapsed : 0,
  };
}

function triggerSpeechCycle(rt: MockRuntime, now: number, handlers: MockHandlers) {
  rt.micState = 'WAITING_TAP';
  rt.micStateStartedAt = now;
  handlers.onMicState('WAITING_TAP');

  // 首轮固定挑最长那条文本，方便用户/截图能稳定看到"长消息折叠"效果；
  // 后续轮次随机挑选。
  const cycleIdx = ++rt.speechCycleCount;
  const text =
    cycleIdx === 1
      ? SPEECH_PHRASES[SPEECH_PHRASES.length - 1]
      : SPEECH_PHRASES[Math.floor(Math.random() * SPEECH_PHRASES.length)];
  const pendingId = makeId();

  // 如果 onAudioChunk 可用，走完整 ASR 链路模拟：
  // RECORDING 期间 emit 假 audio_chunk，final 后由 Hook 的 ASR 逻辑接管
  if (handlers.onAudioChunk) {
    const emitAudioChunks = () => {
      // 生成 3 个假 PCM chunk + 1 个 final chunk（模拟 ~2s 录音）
      const chunkCount = 3;
      for (let seq = 0; seq <= chunkCount; seq++) {
        const isFinal = seq === chunkCount;
        // 生成假 PCM 数据（静音 + 微噪，每块约 0.5s = 16000 samples/s * 0.5s * 2 bytes = 16000 bytes）
        const pcmBytes = isFinal ? 0 : 16000;
        let pcmBase64 = '';
        if (pcmBytes > 0) {
          const buf = new Uint8Array(pcmBytes);
          // 填入微小随机噪声模拟音频
          for (let i = 0; i < pcmBytes; i++) {
            buf[i] = Math.floor(Math.random() * 4); // 接近静音的低幅度噪声
          }
          pcmBase64 = btoa(String.fromCharCode(...buf));
        }
        handlers.onAudioChunk!({
          seq,
          sampleRate: 16000,
          bitsPerSample: 16,
          channels: 1,
          pcmBase64,
          final: isFinal,
        });
      }
    };
    // RECORDING 开始于 1100ms (WAITING_TAP 700 + ARMED 400)，emit chunks
    setTimeout(emitAudioChunks, 3300); // RECORDING 结束时 emit 所有 chunks
    return; // ASR 链路由 Hook 处理 pending/final
  }

  // 降级路径：无 onAudioChunk 回调，保持原有直接 onMessage 行为
  // 在 PROCESSING 阶段开始（约 3300ms 后）插入 pending 占位气泡，
  // 让用户看到"正在识别…"的呼吸动画。
  // 时间线：WAITING_TAP 700 + ARMED 400 + RECORDING 2200 = 3300ms 进入 PROCESSING
  setTimeout(() => {
    handlers.onMessage({
      id: pendingId,
      role: 'speech',
      text: '识别中…',
      timestamp: Date.now(),
      pending: true,
    });
  }, 3300);

  // PROCESSING 1800ms 后用同 id 替换为最终结果
  setTimeout(() => {
    handlers.onMessage({
      id: pendingId,
      role: 'speech',
      text,
      timestamp: Date.now(),
      confidence: 0.88 + Math.random() * 0.1,
      latencyMs: Math.floor(randomBetween(280, 520)),
    });
  }, 5000);
}

// ─── 兼容旧导出 (供测试或预留) ────────────────────────────────────────────
export function generateRandomFlexArray(): FlexArray5 {
  return [
    randomFlexRaw(Math.random() < 0.5),
    randomFlexRaw(Math.random() < 0.5),
    randomFlexRaw(Math.random() < 0.5),
    randomFlexRaw(Math.random() < 0.5),
    randomFlexRaw(Math.random() < 0.5),
  ];
}
