/**
 * CalibrationWizard — 校准引导对话框
 *
 * 状态机由 firmware [CAL] marker 驱动，前端被动渲染：
 *   IDLE → 用户点 "开始校准" → 调 startCalibration → WAITING
 *   收到 stage=imu phase=countdown            → IMU 倒计时
 *   收到 stage=imu phase=sampling             → IMU 采样进度
 *   收到 stage=imu phase=done ok=1            → 切到 flex_min
 *   ...（flex_min / flex_max 同理）
 *   收到 stage=overall phase=done ok=1        → 自动 readCalibrationState 并提示完成
 *   收到任何 ok=0                             → 显示错误 + reason
 *
 * 校准期间 firmware 阻塞 ~21s，无法接收任何串口命令；本对话框不试图取消，
 * 仅允许用户关闭弹窗（板子仍会写完 NVS）。
 *
 * 日志区订阅 'serial-log' 事件（如有；否则展示 wizard 内部状态变化）。
 */
import { useCallback, useEffect, useMemo, useState } from "react";
import { startCalibration, readCalibrationState } from "../api";
import { useCaptureStore } from "../store";
import type { CalProgress, CalStage } from "../types";

interface Props {
  alias: string;
  displayName: string;
}

interface LogEntry {
  ts: number;
  text: string;
  kind: "info" | "warn" | "err";
}

const STAGE_ORDER: CalStage[] = ["imu", "flex_min", "flex_max", "save"];
const STAGE_LABEL: Record<CalStage, string> = {
  overall: "整体",
  imu: "IMU 零偏",
  flex_min: "Flex 最小值（手指伸直）",
  flex_max: "Flex 最大值（握紧拳头）",
  save: "写入 NVS",
};
const STAGE_TIPS: Record<CalStage, string> = {
  overall: "",
  imu: "请把手套平放在桌面，保持静止",
  flex_min: "请把所有手指完全伸直",
  flex_max: "请用力握紧拳头",
  save: "正在写入 NVS…",
};

function fmtTime(ts: number): string {
  const d = new Date(ts);
  return `${String(d.getHours()).padStart(2, "0")}:${String(d.getMinutes()).padStart(2, "0")}:${String(d.getSeconds()).padStart(2, "0")}.${String(d.getMilliseconds()).padStart(3, "0")}`;
}

function describeProgress(p: CalProgress): string {
  const stage = STAGE_LABEL[p.stage] ?? p.stage;
  if (p.phase === "start") return `[${stage}] 开始`;
  if (p.phase === "countdown") return `[${stage}] 倒计时 ${p.remain ?? "?"}`;
  if (p.phase === "sampling") return `[${stage}] 采样中…`;
  if (p.phase === "done") {
    if (p.ok === false) return `[${stage}] 失败：${p.reason ?? "unknown"}`;
    if (p.stage === "overall") {
      return `[${stage}] 完成 (flags=${p.flags ?? 0})`;
    }
    return `[${stage}] 完成 ✓`;
  }
  return `[${stage}] ${p.phase}`;
}

export function CalibrationWizard({ alias, displayName }: Props) {
  const card = useCaptureStore((s) => s.calibrationCards[alias]);
  const closeCalWizard = useCaptureStore((s) => s.closeCalWizard);
  const resetCalCardError = useCaptureStore((s) => s.resetCalCardError);

  const [logs, setLogs] = useState<LogEntry[]>([]);
  const [starting, setStarting] = useState(false);
  const [startedOnce, setStartedOnce] = useState(false);
  const [errMsg, setErrMsg] = useState<string | null>(null);

  // 当 lastProgress 变化时追加日志
  useEffect(() => {
    if (!card?.lastProgress) return;
    const p = card.lastProgress;
    const kind: LogEntry["kind"] =
      p.phase === "done" && p.ok === false ? "err" : "info";
    setLogs((prev) => [
      ...prev,
      { ts: Date.now(), text: describeProgress(p), kind },
    ]);

    // overall done ok=1 → 自动读最新 NVS（让 card 上的 calInfo 同步）
    if (
      p.stage === "overall" &&
      p.phase === "done" &&
      p.ok !== false
    ) {
      readCalibrationState(alias).catch(() => {});
    }
  }, [card?.lastProgress, alias]);

  const handleStart = useCallback(async () => {
    setErrMsg(null);
    setStarting(true);
    setLogs((prev) => [
      ...prev,
      { ts: Date.now(), text: "→ 已发送 r + k 命令，等待 firmware 响应…", kind: "info" },
    ]);
    resetCalCardError(alias);
    try {
      await startCalibration(alias);
      setStartedOnce(true);
    } catch (e) {
      setErrMsg(String(e));
      setLogs((prev) => [
        ...prev,
        { ts: Date.now(), text: `× 启动失败: ${String(e)}`, kind: "err" },
      ]);
    } finally {
      setStarting(false);
    }
  }, [alias, resetCalCardError]);

  const handleClose = useCallback(() => {
    if (card?.wizardState === "running") {
      const ok = window.confirm(
        "板子仍在校准（约 21 秒），关闭窗口不会中断校准，结果会自动写入 NVS。确定关闭？"
      );
      if (!ok) return;
    }
    closeCalWizard(alias);
  }, [alias, card?.wizardState, closeCalWizard]);

  // 当前进度的 stage 在序列中的位置（用于步骤指示器）
  const currentStageIdx = useMemo(() => {
    const p = card?.lastProgress;
    if (!p || p.stage === "overall") return -1;
    return STAGE_ORDER.indexOf(p.stage);
  }, [card?.lastProgress]);

  const finished =
    card?.lastProgress?.stage === "overall" &&
    card?.lastProgress?.phase === "done" &&
    card?.lastProgress?.ok !== false;

  const failed = card?.wizardState === "error";
  const running = card?.wizardState === "running";

  const currentTip = useMemo(() => {
    const p = card?.lastProgress;
    if (!p) return "请按下 \"开始校准\" 按钮";
    if (p.stage === "overall") return "";
    return STAGE_TIPS[p.stage] ?? "";
  }, [card?.lastProgress]);

  const currentStageLabel = useMemo(() => {
    const p = card?.lastProgress;
    if (!p) return "";
    if (p.stage === "overall") return STAGE_LABEL.overall;
    return STAGE_LABEL[p.stage];
  }, [card?.lastProgress]);

  const bigDisplay = useMemo(() => {
    const p = card?.lastProgress;
    if (!p) return "—";
    if (p.phase === "countdown") return String(p.remain ?? "?");
    if (p.phase === "sampling") return "●●●";
    if (p.phase === "done") return p.ok === false ? "✗" : "✓";
    return "…";
  }, [card?.lastProgress]);

  return (
    <div className="cal-wizard__backdrop" role="dialog" aria-modal="true">
      <div className="cal-wizard">
        <div className="cal-wizard__header">
          <span className="cal-wizard__title">
            设备校准 · {displayName}
            <span className="cal-wizard__alias">({alias})</span>
          </span>
          <button
            type="button"
            className="cal-wizard__close"
            onClick={handleClose}
            title="关闭"
          >
            ×
          </button>
        </div>

        <div className="cal-wizard__warning">
          ⚠ 校准期间板子无法响应其他操作，请勿拔插。约 21 秒完成。
        </div>

        <div className="cal-wizard__steps">
          {STAGE_ORDER.map((s, i) => {
            const cls =
              currentStageIdx > i
                ? "cal-step cal-step--done"
                : currentStageIdx === i
                ? "cal-step cal-step--active"
                : "cal-step";
            return (
              <div key={s} className={cls}>
                <span className="cal-step__num">{i + 1}</span>
                <span className="cal-step__name">{STAGE_LABEL[s]}</span>
              </div>
            );
          })}
        </div>

        <div className="cal-wizard__center">
          <div className="cal-wizard__stage-name">{currentStageLabel || "—"}</div>
          <div className="cal-wizard__big">{bigDisplay}</div>
          <div className="cal-wizard__tip">{currentTip}</div>
        </div>

        {finished && (
          <div className="cal-wizard__result cal-wizard__result--ok">
            ✓ 校准完成，已写入 NVS。flags = {card?.lastProgress?.flags ?? 0}
          </div>
        )}
        {failed && (
          <div className="cal-wizard__result cal-wizard__result--err">
            ✗ 校准失败：{card?.lastError ?? "unknown"}
          </div>
        )}

        <div className="cal-wizard__log">
          <div className="cal-wizard__log-title">▽ 串口日志</div>
          <div className="cal-wizard__log-body">
            {logs.length === 0 && (
              <div className="cal-wizard__log-empty">（暂无）</div>
            )}
            {logs.map((l, i) => (
              <div key={i} className={`cal-wizard__log-line cal-wizard__log-line--${l.kind}`}>
                <span className="cal-wizard__log-ts">{fmtTime(l.ts)}</span>
                <span>{l.text}</span>
              </div>
            ))}
          </div>
        </div>

        <div className="cal-wizard__actions">
          {!startedOnce && !running && (
            <button
              type="button"
              className="btn"
              disabled={starting || !card?.connected}
              onClick={handleStart}
            >
              {starting ? "发送中…" : "开始校准"}
            </button>
          )}
          {(startedOnce || running) && !finished && !failed && (
            <button type="button" className="btn" disabled>
              校准进行中…
            </button>
          )}
          {(finished || failed) && (
            <>
              <button
                type="button"
                className="btn btn--ghost"
                onClick={() => {
                  setLogs([]);
                  setStartedOnce(false);
                  resetCalCardError(alias);
                }}
              >
                {failed ? "重试" : "再次校准"}
              </button>
              <button
                type="button"
                className="btn"
                onClick={() => closeCalWizard(alias)}
              >
                关闭
              </button>
            </>
          )}
          {!finished && !failed && (startedOnce || running) && (
            <button
              type="button"
              className="btn btn--ghost"
              onClick={handleClose}
            >
              隐藏窗口
            </button>
          )}
        </div>

        {errMsg && <div className="cal-wizard__err">⚠ {errMsg}</div>}
      </div>
    </div>
  );
}
