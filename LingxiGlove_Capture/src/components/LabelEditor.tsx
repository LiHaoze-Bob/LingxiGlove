/**
 * LabelEditor — Day 5 Label 编辑器
 *
 * - 10 行（label 0-9）固定，不可增删
 * - 单击行：选择该 label 为当前 label（同 LabelHUD 行为）
 * - 双击行：进入 inline 编辑模式（input + Enter 保存 / Esc 取消）
 * - 顶部按钮：「重置默认」
 * - 持久化：localStorage（store 内置）
 */
import { useEffect, useRef, useState } from "react";
import { useCaptureStore } from "../store";

interface Props {
  open: boolean;
  onClose: () => void;
}

export function LabelEditor({ open, onClose }: Props) {
  const labelNames = useCaptureStore((s) => s.labelNames);
  const renameLabel = useCaptureStore((s) => s.renameLabel);
  const resetLabelNames = useCaptureStore((s) => s.resetLabelNames);
  const currentLabel = useCaptureStore((s) => s.currentLabel);
  const setCurrentLabel = useCaptureStore((s) => s.setCurrentLabel);

  const [editing, setEditing] = useState<number | null>(null);
  const [draft, setDraft] = useState("");
  const inputRef = useRef<HTMLInputElement | null>(null);

  // 打开时复位编辑态
  useEffect(() => {
    if (!open) {
      setEditing(null);
      setDraft("");
    }
  }, [open]);

  // 进入编辑态后自动 focus
  useEffect(() => {
    if (editing !== null && inputRef.current) {
      inputRef.current.focus();
      inputRef.current.select();
    }
  }, [editing]);

  if (!open) return null;

  function startEdit(idx: number) {
    setEditing(idx);
    setDraft(labelNames[idx] ?? "");
  }
  function commitEdit() {
    if (editing === null) return;
    const trimmed = draft.trim();
    if (trimmed) renameLabel(editing, trimmed);
    setEditing(null);
    setDraft("");
  }
  function cancelEdit() {
    setEditing(null);
    setDraft("");
  }

  return (
    <div
      className="modal-backdrop"
      role="dialog"
      aria-modal="true"
      aria-label="Label 编辑器"
      onClick={(e) => {
        if (e.target === e.currentTarget) onClose();
      }}
    >
      <div className="modal label-editor">
        <div className="modal__header">
          <span>Label 编辑（共 10 个，0-9）</span>
          <button className="link-btn" type="button" onClick={onClose}>
            ×
          </button>
        </div>

        <div className="modal__hint">
          单击 = 选择 · 双击 = 改名 · Enter 保存 · Esc 取消
        </div>

        <ul className="label-editor__list">
          {labelNames.map((name, idx) => {
            const isEditing = editing === idx;
            const isSelected = currentLabel === idx;
            return (
              <li
                key={idx}
                className={`label-editor__item ${
                  isSelected ? "label-editor__item--selected" : ""
                }`}
                onClick={() => {
                  if (!isEditing) setCurrentLabel(idx);
                }}
                onDoubleClick={() => startEdit(idx)}
              >
                <span className="label-editor__index">{idx}</span>
                {isEditing ? (
                  <input
                    ref={inputRef}
                    type="text"
                    className="label-editor__input"
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
                    }}
                    onBlur={commitEdit}
                    onClick={(e) => e.stopPropagation()}
                    maxLength={32}
                  />
                ) : (
                  <span className="label-editor__name">{name}</span>
                )}
              </li>
            );
          })}
        </ul>

        <div className="modal__footer">
          <button
            type="button"
            className="btn btn--ghost"
            onClick={() => {
              if (confirm("确定恢复默认 10 个 label 名？当前自定义将丢失。")) {
                resetLabelNames();
              }
            }}
          >
            重置默认
          </button>
          <button type="button" className="btn" onClick={onClose}>
            完成
          </button>
        </div>
      </div>
    </div>
  );
}
