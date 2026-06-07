/**
 * 全局键盘热键 hook（Day 5：状态机驱动，编排逻辑下沉到 store）
 *
 * 状态机：DISCONNECTED → HANDSHAKING → IDLE → COUNTDOWN → RECORDING → FINISHING
 *
 * 键位：
 *   - 0..9        →  设置 label = 该数字（IDLE 选 / RECORDING 实时切）
 *   - `-`         →  设置 label = -1（unlabeled）
 *   - Space (IDLE)        →  startCountdownFlow（store 内部链 sendChar 'c' + startSession）
 *   - Space (其它态)      →  忽略
 *   - Enter (RECORDING)   →  stopRecordingFlow
 *   - Esc (COUNTDOWN)     →  cancelCountdown
 *   - Esc (RECORDING)     →  abort：仍调 stopRecordingFlow（保留写盘内容）
 *   - Esc (帮助 overlay)  →  关闭帮助
 *   - ?           →  打开/关闭快捷键帮助 overlay
 *
 * 输入框 / textarea 内的按键放行。
 */
import { useEffect } from "react";
import { useCaptureStore } from "../store";

interface KeydownOptions {
  onToggleHelp?: () => void;
  onCloseHelp?: () => void;
  /** 注入逻辑：当前是否处于"会确认"状态（比如打开了 dialog 时不响应键位） */
  enabled?: boolean;
}

function isEditableTarget(t: EventTarget | null): boolean {
  if (!t || !(t instanceof HTMLElement)) return false;
  const tag = t.tagName;
  if (tag === "INPUT" || tag === "TEXTAREA" || tag === "SELECT") return true;
  if (t.isContentEditable) return true;
  return false;
}

export function useGlobalKeydown(opts: KeydownOptions = {}): void {
  const { onToggleHelp, onCloseHelp, enabled = true } = opts;

  useEffect(() => {
    if (!enabled) return;

    const handler = async (e: KeyboardEvent) => {
      if (isEditableTarget(e.target)) return;
      // 忽略修饰键组合（Cmd / Ctrl / Alt） — 让浏览器/系统快捷键正常工作
      if (e.metaKey || e.ctrlKey || e.altKey) return;

      const k = e.key;
      const store = useCaptureStore.getState();
      const flow = store.captureFlow;

      // 数字 0..9 → label（任何状态都允许：IDLE 预选 / RECORDING 实时切）
      if (k >= "0" && k <= "9") {
        e.preventDefault();
        await store.setCurrentLabel(parseInt(k, 10));
        return;
      }
      // `-` → unlabeled
      if (k === "-") {
        e.preventDefault();
        await store.setCurrentLabel(-1);
        return;
      }

      // Space → IDLE 时进入 COUNTDOWN
      if (k === " " || k === "Space") {
        e.preventDefault();
        if (flow === "IDLE") {
          store.startCountdownFlow();
        }
        return;
      }

      // Enter → RECORDING 时停止
      if (k === "Enter") {
        e.preventDefault();
        if (flow === "RECORDING") {
          await store.stopRecordingFlow();
        }
        return;
      }

      // Esc：COUNTDOWN 取消 / RECORDING abort / 否则关帮助 overlay
      if (k === "Escape") {
        if (flow === "COUNTDOWN") {
          e.preventDefault();
          store.cancelCountdown();
          return;
        }
        if (flow === "RECORDING") {
          e.preventDefault();
          await store.stopRecordingFlow();
          return;
        }
        onCloseHelp?.();
        return;
      }

      // 帮助 overlay
      if (k === "?") {
        e.preventDefault();
        onToggleHelp?.();
        return;
      }
    };

    window.addEventListener("keydown", handler);
    return () => {
      window.removeEventListener("keydown", handler);
    };
  }, [enabled, onToggleHelp, onCloseHelp]);
}
