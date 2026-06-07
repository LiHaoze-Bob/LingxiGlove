'use client';

import { useEffect, useState } from 'react';
import {
  FINGER_NAMES,
  FingerKey,
  HandSide,
  HandTelemetry,
  MicState,
  SystemSnapshot,
} from '@/lib/types';

interface Props {
  snapshot: SystemSnapshot;
}

/**
 * 工程仪表板 (右侧 1/3 区域)
 * 卡片顺序按用户视线焦点编排：
 *   GESTURE（识别结果，第一焦点）
 *   MIC / PTT（当前交互态）
 *   FLEX × 5（传感器底层数据）
 *   SYSTEM（系统健康，沉到最后）
 */
export function Dashboard({ snapshot }: Props) {
  return (
    <aside className="dashboard">
      <DashboardHeader snapshot={snapshot} />
      <GestureCard snapshot={snapshot} />
      <MicCard snapshot={snapshot} />
      <FlexCard snapshot={snapshot} />
      <SystemCard snapshot={snapshot} />
    </aside>
  );
}

// ─── 顶部状态条 ───────────────────────────────────────────────────────────
function DashboardHeader({ snapshot }: Props) {
  const { connectionStatus, ip, uptimeSec } = snapshot.system;
  const dot =
    connectionStatus === 'connected'
      ? 'dot-online'
      : connectionStatus === 'reconnecting'
      ? 'dot-warn'
      : 'dot-offline';
  const label =
    connectionStatus === 'connected'
      ? 'SYSTEM ONLINE'
      : connectionStatus === 'reconnecting'
      ? 'RECONNECTING'
      : 'OFFLINE';
  return (
    <div className="dash-header">
      <span className={`dash-dot ${dot}`} />
      <span className="dash-header-label">{label}</span>
      <span className="dash-header-meta">{ip}</span>
      <span className="dash-header-meta">UP {formatUptime(uptimeSec)}</span>
    </div>
  );
}

function formatUptime(sec: number): string {
  const h = Math.floor(sec / 3600);
  const m = Math.floor((sec % 3600) / 60);
  const s = sec % 60;
  return `${h.toString().padStart(2, '0')}:${m.toString().padStart(2, '0')}:${s
    .toString()
    .padStart(2, '0')}`;
}

// ─── Card 容器 ───────────────────────────────────────────────────────────
function Card({
  title,
  badge,
  children,
}: {
  title: string;
  badge?: string;
  children: React.ReactNode;
}) {
  return (
    <section className="dash-card">
      <header className="dash-card-header">
        <span className="dash-card-title">{title}</span>
        {badge && <span className="dash-card-badge">{badge}</span>}
      </header>
      <div className="dash-card-body">{children}</div>
    </section>
  );
}

// ─── SYSTEM ──────────────────────────────────────────────────────────────
function SystemCard({ snapshot }: Props) {
  const { rssi, battery, latencyMs, packetRate } = snapshot.system;
  const rssiBars = rssiToBars(rssi);
  const batColor =
    battery > 50 ? '#00ff88' : battery > 20 ? '#ffaa33' : '#ff5555';
  return (
    <Card title="SYSTEM" badge="HEALTH">
      <div className="metric-grid">
        <Metric label="WiFi" value={`${rssi} dBm`}>
          <div className="rssi-bars">
            {[1, 2, 3, 4].map((b) => (
              <span
                key={b}
                className={`rssi-bar ${b <= rssiBars ? 'active' : ''}`}
                style={{ height: `${b * 22}%` }}
              />
            ))}
          </div>
        </Metric>
        <Metric label="BATT" value={`${battery.toFixed(0)}%`}>
          <div className="bat">
            <div
              className="bat-fill"
              style={{ width: `${battery}%`, background: batColor }}
            />
          </div>
        </Metric>
        <Metric label="LATENCY" value={`${latencyMs} ms`} />
        <Metric label="PACKETS" value={`${packetRate}/s`} />
      </div>
    </Card>
  );
}

function rssiToBars(rssi: number): number {
  if (rssi >= -55) return 4;
  if (rssi >= -65) return 3;
  if (rssi >= -75) return 2;
  if (rssi >= -85) return 1;
  return 0;
}

function Metric({
  label,
  value,
  children,
}: {
  label: string;
  value: string;
  children?: React.ReactNode;
}) {
  return (
    <div className="metric">
      <div className="metric-label">{label}</div>
      <div className="metric-value">{value}</div>
      {children}
    </div>
  );
}

// ─── FLEX × 5 ────────────────────────────────────────────────────────────
function FlexCard({ snapshot }: Props) {
  return (
    <Card title="FLEX × 5" badge="L · R">
      <div className="flex-bihands">
        <HandColumn side="left" label="LEFT" hand={snapshot.hands.left} />
        <div className="flex-divider" />
        <HandColumn side="right" label="RIGHT" hand={snapshot.hands.right} />
      </div>
    </Card>
  );
}

function HandColumn({
  side,
  label,
  hand,
}: {
  side: HandSide;
  label: string;
  hand: HandTelemetry;
}) {
  return (
    <div className={`flex-hand-col flex-hand-${side}`}>
      <div className="flex-hand-head">
        <span className="flex-hand-label">{label}</span>
        <span className="flex-hand-finger-legend">拇 食 中 无 小</span>
      </div>
      <div className="flex-bars">
        {(FINGER_NAMES as readonly FingerKey[]).map((f) => {
          const st = hand.fingers[f];
          const pct = Math.round(st.normalized * 100);
          return (
            <div key={f} className="flex-bar-row">
              <div className="flex-bar-name">{f}</div>
              <div className="flex-bar-track">
                <div
                  className={`flex-bar-fill ${st.bent ? 'bent' : ''}`}
                  style={{ width: `${pct}%` }}
                />
                <div className="flex-bar-threshold" />
              </div>
              <div className="flex-bar-val">{st.raw}</div>
            </div>
          );
        })}
      </div>
      <div className="imu-row imu-row-compact">
        <div className="imu-cell">
          <div className="imu-label">P</div>
          <div className="imu-val">{hand.imu.pitch.toFixed(0)}°</div>
        </div>
        <div className="imu-cell">
          <div className="imu-label">R</div>
          <div className="imu-val">{hand.imu.roll.toFixed(0)}°</div>
        </div>
        <div className="imu-cell">
          <div className="imu-label">|ω|</div>
          <div className="imu-val">
            {hand.imu.gyroMag?.toFixed(0) ?? 0}
          </div>
        </div>
      </div>
    </div>
  );
}

// ─── MIC ─────────────────────────────────────────────────────────────────
// 录音时长阈值（ms）—— 与端侧 WS_MIC_STREAM_MAX_MS（55s）保持一致：
//   < REC_WARN_MS       绿色：正常
//   REC_WARN_MS~MAX_MS  黄色：接近上限，请尽快说完
//   ≥ REC_MAX_MS        红色 + 警告：端侧 watchdog 即将触发自动停止
const REC_WARN_MS = 30000;
const REC_MAX_MS  = 50000;

function MicCard({ snapshot }: Props) {
  const { state, level, spectrum, recordingMs } = snapshot.mic;
  // ─── 电平平滑（避免 5Hz snapshot 的瞬时电平上下乱跳）────────────────
  // EMA：alpha 越小越平滑；非 RECORDING 立即归零，避免余晖。
  const [smoothLevel, setSmoothLevel] = useState(0);
  useEffect(() => {
    if (state !== 'RECORDING') {
      setSmoothLevel(0);
      return;
    }
    setSmoothLevel((prev) => prev * 0.7 + level * 0.3);
  }, [state, level]);
  const stateColor: Record<MicState, string> = {
    IDLE: '#5a5a7a',
    WAITING_TAP: '#ffaa33',
    ARMED: '#ff6b35',
    RECORDING: '#ff3355',
    PROCESSING: '#00e5ff',
  };
  // 录音计时器配色 + 提示文案
  const recTimeColor =
    recordingMs >= REC_MAX_MS
      ? '#ff3355'
      : recordingMs >= REC_WARN_MS
      ? '#ffaa33'
      : '#00ff88';
  const recTimeHint =
    recordingMs >= REC_MAX_MS
      ? '即将自动停止 ⚠'
      : recordingMs >= REC_WARN_MS
      ? '请尽快说完'
      : '';
  return (
    <Card title="MIC / PTT" badge={state}>
      <div className="mic-state-line">
        <span
          className="mic-state-dot"
          style={{ background: stateColor[state] }}
        />
        <span className="mic-state-label">{describeMicState(state)}</span>
        {/* 录音计时槽位始终渲染 — 非 RECORDING 时 visibility:hidden 占位保高，
           避免该 span 出现/消失导致 mic-state-line 在录音过程中反复抖动。 */}
        <span
          className="mic-rec-time"
          style={{
            visibility: state === 'RECORDING' ? 'visible' : 'hidden',
            color: recTimeColor,
            fontWeight: recordingMs >= REC_MAX_MS ? 700 : 500,
            animation:
              state === 'RECORDING' && recordingMs >= REC_MAX_MS
                ? 'mic-rec-pulse 0.8s infinite'
                : undefined,
          }}
        >
          {(recordingMs / 1000).toFixed(1)}s
          {state === 'RECORDING' && recTimeHint && (
            <span className="mic-rec-hint" style={{ marginLeft: 6, fontSize: '0.85em' }}>
              {recTimeHint}
            </span>
          )}
        </span>
      </div>
      <div className="mic-spectrum">
        {spectrum.map((v, i) => (
          <div
            key={i}
            className="mic-bar"
            style={{ height: `${4 + v * 96}%`, opacity: 0.4 + v * 0.6 }}
          />
        ))}
      </div>
      <div className="mic-level-row">
        <span className="mic-level-label">LEVEL</span>
        <div className="mic-level-track">
          <div
            className="mic-level-fill"
            style={{ width: `${smoothLevel * 100}%` }}
          />
        </div>
      </div>
    </Card>
  );
}

function describeMicState(s: MicState): string {
  switch (s) {
    case 'IDLE':
      return '待命中（双击腕部启动 / 握拳结束）';
    case 'WAITING_TAP':
      return '等待双击启动…';
    case 'ARMED':
      return '检测到第 1 击，等第 2 击…';
    case 'RECORDING':
      return '正在录音 ●（握拳即可结束）';
    case 'PROCESSING':
      return 'ASR 识别中…';
  }
}

// ─── GESTURE ─────────────────────────────────────────────────────────────
// 候选区固定 3 行：端侧实际只推 top1，mock 推 3 条，cache 过期时 0 条 —
// 行数浮动会让 GestureCard 高度抖动，连带把下方 MIC/FLEX/SYSTEM 卡片推得闪烁。
// 不足 3 行用 dim 占位补齐，整张卡高度恒定。
const GESTURE_SLOT_COUNT = 3;

// 端侧 snapshot.gesture 受 kGestureTtlMs 控制：仲裁器播报后约 1~2s 候选就清空，
// 文字一闪而过，无法回看上一次结果。为此在卡片内部缓存最近一次有效识别，
// 当 candidates 为空时回填展示（淡化 + "保留" 标记），新识别到来时立即替换。
type GestureMemo = {
  candidates: SystemSnapshot['gesture']['candidates'];
  source: SystemSnapshot['gesture']['source'];
  timestamp: number;
};

function GestureCard({ snapshot }: Props) {
  const live = snapshot.gesture;
  const [memo, setMemo] = useState<GestureMemo | null>(null);
  useEffect(() => {
    if (live.candidates.length > 0) {
      setMemo({
        candidates: live.candidates,
        source: live.source,
        timestamp: live.timestamp,
      });
    }
  }, [live.candidates, live.source, live.timestamp]);

  // live 优先；live 无数据则回放 memo（淡化），都没有显示初始 "等待识别"
  const isStale = live.candidates.length === 0 && memo != null;
  const candidates = live.candidates.length > 0 ? live.candidates : memo?.candidates ?? [];
  const source = live.candidates.length > 0 ? live.source : memo?.source ?? live.source;
  const top = candidates[0];
  const placeholder = !top;

  const slots: (typeof candidates[number] | null)[] = [];
  for (let i = 0; i < GESTURE_SLOT_COUNT; i++) {
    slots.push(candidates[i] ?? null);
  }
  return (
    <Card
      title="GESTURE"
      badge={isStale ? `${source.toUpperCase()} · 保留` : source.toUpperCase()}
    >
      <div className="gesture-top" style={isStale ? { opacity: 0.55 } : undefined}>
        <div className="gesture-top-label">TOP</div>
        <div
          className="gesture-top-text"
          style={placeholder ? { color: 'var(--text-mute)' } : undefined}
        >
          {top ? top.text : '—'}
        </div>
        <div className="gesture-top-conf">
          {top ? `${(top.confidence * 100).toFixed(0)}%` : '—'}
        </div>
      </div>
      <div className="gesture-cands" style={isStale ? { opacity: 0.55 } : undefined}>
        <div className="gesture-cands-legend" aria-hidden="true">
          <span className="gesture-cand-rank" />
          <span className="gesture-cand-text">候选</span>
          <span className="gesture-cand-track-label">置信度</span>
          <span className="gesture-cand-conf">CONF</span>
        </div>
        {slots.map((c, i) => {
          // i==0 且无数据 → 占位 "等待识别"；其余空槽 → 完全空白行（仅占位）
          const isFirstEmpty = i === 0 && !c;
          return (
            <div
              key={i}
              className="gesture-cand"
              style={
                !c
                  ? { opacity: isFirstEmpty ? 0.4 : 0.15 }
                  : undefined
              }
            >
              <span className="gesture-cand-rank">#{i + 1}</span>
              <span className="gesture-cand-text">
                {c ? c.text : isFirstEmpty ? '等待识别' : '—'}
              </span>
              <div
                className="gesture-cand-track"
                title={c ? `置信度 ${(c.confidence * 100).toFixed(1)}%` : undefined}
              >
                <div
                  className="gesture-cand-fill"
                  style={{ width: c ? `${c.confidence * 100}%` : '0%' }}
                />
              </div>
              <span className="gesture-cand-conf">
                {c ? `${(c.confidence * 100).toFixed(0)}%` : '—'}
              </span>
            </div>
          );
        })}
      </div>
    </Card>
  );
}
