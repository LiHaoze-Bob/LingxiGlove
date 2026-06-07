/**
 * KeyHelp — `?` 触发的快捷键帮助 overlay
 */

interface KeyHelpProps {
  open: boolean;
  onClose: () => void;
}

const ROWS: { key: string; desc: string }[] = [
  { key: "0 .. 9", desc: "设置当前 label = 该数字" },
  { key: "-", desc: "回到 unlabeled (-1)" },
  { key: "Space", desc: "开始 / 暂停 / 继续 录制" },
  { key: "Enter", desc: "停止录制（弹会话摘要）" },
  { key: "R", desc: "暂停时继续录制" },
  { key: "?", desc: "切换本帮助面板" },
  { key: "Esc", desc: "关闭帮助" },
];

export function KeyHelp({ open, onClose }: KeyHelpProps) {
  if (!open) return null;
  return (
    <div className="overlay" onClick={onClose}>
      <div className="overlay__panel" onClick={(e) => e.stopPropagation()}>
        <h3>快捷键</h3>
        <table className="kbtable">
          <tbody>
            {ROWS.map((r) => (
              <tr key={r.key}>
                <td>
                  <kbd>{r.key}</kbd>
                </td>
                <td>{r.desc}</td>
              </tr>
            ))}
          </tbody>
        </table>
        <button type="button" className="btn" onClick={onClose}>
          关闭 (Esc)
        </button>
      </div>
    </div>
  );
}
