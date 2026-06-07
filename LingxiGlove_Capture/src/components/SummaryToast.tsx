/**
 * SummaryToast — 会话结束后弹出的摘要（行数 + csv 路径 + 复制按钮）
 */
import { useCaptureStore } from "../store";

export function SummaryToast() {
  const summary = useCaptureStore((s) => s.lastSummary);
  const clearSummary = useCaptureStore((s) => s.clearSummary);

  if (!summary) return null;
  const seconds = (summary.duration_ms / 1000).toFixed(1);

  return (
    <div className="overlay" onClick={clearSummary}>
      <div className="overlay__panel" onClick={(e) => e.stopPropagation()}>
        <h3>✅ 会话已保存</h3>
        <p>
          <strong>{summary.session_id}</strong> · 时长 {seconds}s
        </p>
        <table className="summary">
          <thead>
            <tr>
              <th>设备</th>
              <th>行数</th>
              <th>已打标</th>
              <th>路径</th>
            </tr>
          </thead>
          <tbody>
            {summary.per_device.map((d) => (
              <tr key={d.alias}>
                <td>{d.alias.toUpperCase()}</td>
                <td>{d.rows}</td>
                <td>{d.labeled_rows}</td>
                <td>
                  <code>{d.csv_path}</code>
                </td>
              </tr>
            ))}
          </tbody>
        </table>
        <p className="summary__hint">
          下一步：在终端跑 <code>python tools/build_dataset.py --in &lt;output 根&gt;</code>{" "}
          切窗，然后上传 Edge Impulse Studio。
        </p>
        <button type="button" className="btn" onClick={clearSummary}>
          确定
        </button>
      </div>
    </div>
  );
}
