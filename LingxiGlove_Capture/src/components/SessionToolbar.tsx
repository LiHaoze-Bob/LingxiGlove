/**
 * SessionToolbar — 录制控制条（Day 5：状态机驱动）
 *
 * 按钮直接调 store 的 startCountdownFlow / stopRecordingFlow / cancelCountdown，
 * 与 useGlobalKeydown 共用同一套录制流，避免双套路径。
 *
 * 按 captureFlow 渲染按钮：
 *   - IDLE       → 「开始录制」（空格）
 *   - COUNTDOWN  → 「取消」（Esc）
 *   - RECORDING  → 「停止」（回车）
 *   - FINISHING  → 灰按钮 disabled
 *   - 其它态（HANDSHAKING / DISCONNECTED）→ 「未连接」 disabled
 */
import { useCaptureStore } from "../store";

export function SessionToolbar() {
  const sessionInfo = useCaptureStore((s) => s.sessionInfo);
  const captureFlow = useCaptureStore((s) => s.captureFlow);
  const startCountdownFlow = useCaptureStore((s) => s.startCountdownFlow);
  const stopRecordingFlow = useCaptureStore((s) => s.stopRecordingFlow);
  const cancelCountdown = useCaptureStore((s) => s.cancelCountdown);

  const recording = captureFlow === "RECORDING";
  const totalRows = Object.values(sessionInfo.rows_per_device).reduce(
    (a, b) => a + b,
    0
  );

  const dotClass =
    captureFlow === "RECORDING"
      ? "toolbar__dot--rec"
      : captureFlow === "COUNTDOWN"
        ? "toolbar__dot--paused"
        : "toolbar__dot--idle";

  let statusText: string;
  switch (captureFlow) {
    case "DISCONNECTED":
      statusText = "未连接";
      break;
    case "HANDSHAKING":
      statusText = "握手中…";
      break;
    case "IDLE":
      statusText = "就绪";
      break;
    case "COUNTDOWN":
      statusText = "倒计时中…";
      break;
    case "RECORDING":
      statusText = sessionInfo.session_id ?? "录制中";
      break;
    case "FINISHING":
      statusText = "停止中…";
      break;
  }

  return (
    <div className="toolbar">
      <div className="toolbar__status">
        <span className={"toolbar__dot " + dotClass} />
        <span className="toolbar__session">{statusText}</span>
        {recording && (
          <span className="toolbar__rows">
            {totalRows} 行 ·{" "}
            {Object.entries(sessionInfo.rows_per_device)
              .map(([a, n]) => `${a}=${n}`)
              .join(" ")}
          </span>
        )}
      </div>
      <div className="toolbar__buttons">
        {captureFlow === "IDLE" && (
          <button
            className="btn btn--primary"
            type="button"
            onClick={() => startCountdownFlow()}
          >
            ▶ 开始录制 <kbd>Space</kbd>
          </button>
        )}
        {captureFlow === "COUNTDOWN" && (
          <button
            className="btn"
            type="button"
            onClick={() => cancelCountdown()}
          >
            ✕ 取消 <kbd>Esc</kbd>
          </button>
        )}
        {captureFlow === "RECORDING" && (
          <button
            className="btn btn--danger"
            type="button"
            onClick={() => stopRecordingFlow().catch(console.error)}
          >
            ⏹ 停止 <kbd>Enter</kbd>
          </button>
        )}
        {captureFlow === "FINISHING" && (
          <button className="btn" type="button" disabled>
            停止中…
          </button>
        )}
        {(captureFlow === "DISCONNECTED" ||
          captureFlow === "HANDSHAKING") && (
          <button className="btn" type="button" disabled>
            未就绪
          </button>
        )}
      </div>
    </div>
  );
}
