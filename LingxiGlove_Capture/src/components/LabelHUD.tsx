/**
 * LabelHUD — 大字号显示当前 label，提供按钮快捷切换（与键盘热键互补）
 *
 * Day 5：
 *  - 单击按钮：切当前 label（同键盘 0-9）
 *  - 双击 0-9 按钮：inline 进入改名模式（Enter 保存 / Esc 取消 / blur 取消）
 *  - -1（unlabeled）不可改名
 */
import { useEffect, useRef, useState } from "react";
import { useCaptureStore } from "../store";

const QUICK_LABELS = [-1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9];

export function LabelHUD() {
  const currentLabel = useCaptureStore((s) => s.currentLabel);
  const setCurrentLabel = useCaptureStore((s) => s.setCurrentLabel);
  const labelNames = useCaptureStore((s) => s.labelNames);
  const renameLabel = useCaptureStore((s) => s.renameLabel);

  const [editingIndex, setEditingIndex] = useState<number | null>(null);
  const [draft, setDraft] = useState("");
  const inputRef = useRef<HTMLInputElement | null>(null);

  useEffect(() => {
    if (editingIndex !== null) {
      inputRef.current?.focus();
      inputRef.current?.select();
    }
  }, [editingIndex]);

  const nameOf = (l: number): string => {
    if (l === -1) return "unlabeled";
    if (l >= 0 && l < labelNames.length) return labelNames[l];
    return `label${l}`;
  };

  const beginEdit = (l: number) => {
    if (l < 0 || l > 9) return;
    setEditingIndex(l);
    setDraft(labelNames[l] ?? "");
  };

  const commitEdit = () => {
    if (editingIndex === null) return;
    const v = draft.trim();
    if (v) renameLabel(editingIndex, v);
    setEditingIndex(null);
  };

  const cancelEdit = () => setEditingIndex(null);

  return (
    <div className="hud">
      <div className="hud__main">
        <span className="hud__caption">当前 LABEL</span>
        <span
          className="hud__value"
          data-mode={currentLabel < 0 ? "unlabeled" : "labeled"}
        >
          {currentLabel}
        </span>
        <span className="hud__name">{nameOf(currentLabel)}</span>
      </div>
      <div className="hud__buttons">
        {QUICK_LABELS.map((l) => {
          const isEditing = editingIndex === l;
          return (
            <button
              key={l}
              type="button"
              className={
                "hud__btn" + (currentLabel === l ? " hud__btn--active" : "")
              }
              onClick={() => {
                if (!isEditing) setCurrentLabel(l);
              }}
              onDoubleClick={(e) => {
                e.preventDefault();
                beginEdit(l);
              }}
              title={l < 0 ? "键盘 -" : `键盘 ${l}（双击改名）`}
            >
              {l < 0 ? "-" : l}
              {isEditing ? (
                <input
                  ref={inputRef}
                  className="hud__btn-input"
                  value={draft}
                  onChange={(e) => setDraft(e.target.value)}
                  onKeyDown={(e) => {
                    if (e.key === "Enter") {
                      e.preventDefault();
                      commitEdit();
                    } else if (e.key === "Escape") {
                      e.preventDefault();
                      cancelEdit();
                    }
                    // 阻止全局 keydown（数字 / 空格）夺权
                    e.stopPropagation();
                  }}
                  onBlur={cancelEdit}
                  onClick={(e) => e.stopPropagation()}
                  onDoubleClick={(e) => e.stopPropagation()}
                />
              ) : (
                <em>{nameOf(l)}</em>
              )}
            </button>
          );
        })}
      </div>
    </div>
  );
}
