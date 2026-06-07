/**
 * CountdownOverlay — Day 5 录制启动倒计时
 *
 * 触发：用户在 IDLE 按 Space → store.captureFlow = "COUNTDOWN"
 * 行为：3 → 2 → 1 → GO!（每秒一拍，每拍 800Hz beep；GO 用 1200Hz）
 *      GO 结束 100ms 后切到 RECORDING（由 useGlobalKeydown 调度）
 *
 * 取消：useGlobalKeydown 监听 Esc，setCaptureFlow("IDLE")
 */
import { useEffect, useRef } from "react";
import { useCaptureStore } from "../store";

let sharedCtx: AudioContext | null = null;

function ensureAudioCtx(): AudioContext | null {
  if (typeof window === "undefined") return null;
  if (sharedCtx && sharedCtx.state !== "closed") return sharedCtx;
  try {
    const Ctor =
      (window as unknown as { AudioContext?: typeof AudioContext })
        .AudioContext ??
      (window as unknown as { webkitAudioContext?: typeof AudioContext })
        .webkitAudioContext;
    if (!Ctor) return null;
    sharedCtx = new Ctor();
    return sharedCtx;
  } catch (e) {
    console.warn("AudioContext init failed:", e);
    return null;
  }
}

/** 短促 beep：默认 800Hz / 80ms（GO 用 1200Hz / 150ms） */
export function beep(freq = 800, durationMs = 80, volume = 0.25) {
  const ctx = ensureAudioCtx();
  if (!ctx) return;
  if (ctx.state === "suspended") {
    ctx.resume().catch(() => {});
  }
  const osc = ctx.createOscillator();
  const gain = ctx.createGain();
  osc.type = "sine";
  osc.frequency.value = freq;
  // 起伏包络，避免咔嚓声
  const now = ctx.currentTime;
  gain.gain.setValueAtTime(0, now);
  gain.gain.linearRampToValueAtTime(volume, now + 0.01);
  gain.gain.linearRampToValueAtTime(0, now + durationMs / 1000);
  osc.connect(gain).connect(ctx.destination);
  osc.start(now);
  osc.stop(now + durationMs / 1000 + 0.02);
}

export function CountdownOverlay() {
  const flow = useCaptureStore((s) => s.captureFlow);
  const countdown = useCaptureStore((s) => s.countdown);
  const lastBeepRef = useRef<number>(-1);

  useEffect(() => {
    if (flow !== "COUNTDOWN") {
      lastBeepRef.current = -1;
      return;
    }
    if (countdown === lastBeepRef.current) return;
    lastBeepRef.current = countdown;
    if (countdown === 0) {
      beep(1200, 150, 0.3); // GO!
    } else if (countdown > 0) {
      beep(800, 80, 0.25);
    }
  }, [flow, countdown]);

  if (flow !== "COUNTDOWN") return null;

  const display = countdown === 0 ? "GO!" : String(countdown);
  const isGo = countdown === 0;

  return (
    <div className="countdown-overlay" role="status" aria-live="assertive">
      <div className={`countdown-overlay__num ${isGo ? "countdown-overlay__num--go" : ""}`}>
        {display}
      </div>
      <div className="countdown-overlay__hint">
        准备就绪后开始动作 · 按 Esc 取消
      </div>
    </div>
  );
}
