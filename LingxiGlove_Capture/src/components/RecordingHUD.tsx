/**
 * RecordingHUD — Day 5 录制中浮层
 *
 * 显示：
 *   - 当前 label 名（大号，来自 store.labelNames）
 *   - 已录帧数（来自 sessionInfo.rows_per_device）
 *   - 已录秒数（实时计时器，从 recordingStartedAt 起算）
 *   - slave_age_ms 颜色徽章（绿 <50 / 黄 <200 / 红 >=200 或 -1）
 *
 * 整屏红色脉冲边框（由 .recording-hud__pulse 动画提供）。
 *
 * slave_age_ms 来源：
 *   - 双手 29 列 raw 行的第 27 列（master 帧的 raw_line 携带整行）
 *   - 直接 listen("frame") 取 alias='left' 的最新 raw_line，避免父组件传 prop
 */
import { useEffect, useRef, useState } from "react";
import { listen } from "@tauri-apps/api/event";
import { useCaptureStore } from "../store";
import type { Frame } from "../types";

/**
 * 解析 slave_age_ms：
 * - 双手 29 列 raw 行：ts, m_ax..m_flex4(13), s_ax..s_flex4(13), slave_age, label
 *   → 第 27 列（索引 27）就是 slave_age_ms
 * - master frame 的 raw_line 在 bimanual 路径下 = 整行 29 列
 */
function parseSlaveAgeFromRaw(raw?: string): number | null {
  if (!raw) return null;
  const cols = raw.split(",").map((s) => s.trim());
  if (cols.length !== 29) return null;
  const v = Number(cols[27]);
  return Number.isFinite(v) ? v : null;
}

function ageBadgeClass(age: number | null): string {
  if (age === null) return "age-badge age-badge--none";
  if (age < 0) return "age-badge age-badge--red";
  if (age < 50) return "age-badge age-badge--green";
  if (age < 200) return "age-badge age-badge--yellow";
  return "age-badge age-badge--red";
}

export function RecordingHUD() {
  const flow = useCaptureStore((s) => s.captureFlow);
  const currentLabel = useCaptureStore((s) => s.currentLabel);
  const labelNames = useCaptureStore((s) => s.labelNames);
  const sessionInfo = useCaptureStore((s) => s.sessionInfo);
  const startedAt = useCaptureStore((s) => s.recordingStartedAt);

  const lastMasterRawRef = useRef<string>("");
  const [, force] = useState(0);

  // 监听 master frame，取 raw_line 的第 27 列做 slave_age 显示
  useEffect(() => {
    let unlisten: (() => void) | null = null;
    listen<Frame>("frame", (event) => {
      const f = event.payload;
      if (f.dev_alias === "left" && f.raw_line) {
        lastMasterRawRef.current = f.raw_line;
      }
    }).then((u) => (unlisten = u));
    return () => {
      if (unlisten) unlisten();
    };
  }, []);

  // 200ms 强刷一次（更新已录秒数 + slave_age 徽章）
  useEffect(() => {
    if (flow !== "RECORDING") return;
    const t = setInterval(() => force((n) => n + 1), 200);
    return () => clearInterval(t);
  }, [flow]);

  if (flow !== "RECORDING") return null;

  const labelName =
    currentLabel === -1
      ? "(unlabeled)"
      : currentLabel >= 0 && currentLabel < labelNames.length
      ? labelNames[currentLabel]
      : `label_${currentLabel}`;

  const rowsLeft = sessionInfo.rows_per_device["left"] ?? 0;
  const rowsRight = sessionInfo.rows_per_device["right"] ?? 0;
  const rowsBi = sessionInfo.rows_per_device["bimanual"] ?? 0;
  const rows = Math.max(rowsLeft, rowsRight, rowsBi);

  const elapsedMs = startedAt > 0 ? performance.now() - startedAt : 0;
  const elapsedSec = elapsedMs / 1000;

  const age = parseSlaveAgeFromRaw(lastMasterRawRef.current);

  return (
    <>
      <div className="recording-hud__pulse" aria-hidden="true" />
      <div className="recording-hud" role="status" aria-live="polite">
        <div className="recording-hud__row">
          <span className="recording-hud__dot" /> REC
        </div>
        <div className="recording-hud__label">
          {currentLabel === -1 ? "—" : currentLabel} · {labelName}
        </div>
        <div className="recording-hud__metrics">
          <span>{elapsedSec.toFixed(1)}s</span>
          <span>{rows.toLocaleString()} frames</span>
          <span className={ageBadgeClass(age)}>
            slave {age === null ? "—" : age < 0 ? "lost" : `${age}ms`}
          </span>
        </div>
        <div className="recording-hud__hint">
          按 <kbd>Enter</kbd> 停止 · <kbd>0-9</kbd> 切 label · <kbd>Esc</kbd> 取消
        </div>
      </div>
    </>
  );
}
