/**
 * StatsCard — 单设备运行时统计
 *
 * Day 1 显示：
 *   - alias / port / 状态
 *   - 累计帧数 + 实时 fps
 *   - 最后一帧 raw 预览（让肉眼能看到通路在跑）
 *
 * Day 5：支持 displayName prop（Master / Slave 覆盖 LEFT / RIGHT）
 */
import type { DeviceMeta, FpsSnapshot, Frame } from "../types";

interface Props {
  meta?: DeviceMeta;
  fps?: FpsSnapshot;
  lastFrame?: Frame;
  /** UI 显示名（默认 alias.toUpperCase()，Day 5 用 "Master"/"Slave" 覆盖） */
  displayName?: string;
}

export function StatsCard({ meta, fps, lastFrame, displayName }: Props) {
  const status = meta?.status ?? "idle";

  return (
    <div className="stats__card">
      <div className="stats__title">
        <span>
          <span className={`dot dot--${status}`} />
          {displayName ?? meta?.alias?.toUpperCase() ?? "—"}
        </span>
        <span className="stats__fps">
          {fps ? `${fps.fps.toFixed(1)} fps` : "— fps"}
        </span>
      </div>

      <div className="stats__row">
        <span>port</span>
        <span>{meta?.port ?? "—"}</span>
      </div>
      <div className="stats__row">
        <span>baud</span>
        <span>{meta?.baud ?? "—"}</span>
      </div>
      <div className="stats__row">
        <span>frames</span>
        <span>{fps?.frame_count?.toLocaleString() ?? "0"}</span>
      </div>
      <div className="stats__row">
        <span>channels</span>
        <span>{lastFrame ? lastFrame.values.length : "—"}</span>
      </div>
      <div className="stats__row">
        <span>last raw</span>
        <span
          style={{
            fontFamily: "ui-monospace, monospace",
            fontSize: 11,
            color: "#94a3b8",
            maxWidth: 260,
            overflow: "hidden",
            textOverflow: "ellipsis",
            whiteSpace: "nowrap",
          }}
          title={lastFrame?.raw_line}
        >
          {lastFrame?.raw_line ?? "—"}
        </span>
      </div>
    </div>
  );
}
