'use client';

import { useEffect, useMemo, useRef, useState } from 'react';
import { ConversationMessage } from '@/lib/types';

interface Props {
  messages: ConversationMessage[];
}

/** 长消息折叠阈值（字符数） */
const FOLD_THRESHOLD = 80;
/** 跨多少 ms 显示一次时间分隔条（同日内） */
const DIVIDER_GAP_MS = 5 * 60 * 1000;

function pad2(n: number) {
  return n.toString().padStart(2, '0');
}

function formatTime(ts: number): string {
  const d = new Date(ts);
  return `${pad2(d.getHours())}:${pad2(d.getMinutes())}:${pad2(d.getSeconds())}`;
}

/**
 * 时间分隔条文案：
 *  - 今天的消息且与上一条间隔 ≥ 5 分钟 → "HH:mm"
 *  - 昨天 → "昨天 HH:mm"
 *  - 更早 → "YYYY-MM-DD HH:mm"
 *  - 第一条消息总是显示一个分隔（startOfList=true）
 */
function formatDivider(ts: number): string {
  const d = new Date(ts);
  const today = new Date();
  const isSameDay =
    d.getFullYear() === today.getFullYear() &&
    d.getMonth() === today.getMonth() &&
    d.getDate() === today.getDate();
  if (isSameDay) {
    return `${pad2(d.getHours())}:${pad2(d.getMinutes())}`;
  }
  const yesterday = new Date(today);
  yesterday.setDate(today.getDate() - 1);
  const isYesterday =
    d.getFullYear() === yesterday.getFullYear() &&
    d.getMonth() === yesterday.getMonth() &&
    d.getDate() === yesterday.getDate();
  if (isYesterday) {
    return `昨天 ${pad2(d.getHours())}:${pad2(d.getMinutes())}`;
  }
  return `${d.getFullYear()}-${pad2(d.getMonth() + 1)}-${pad2(d.getDate())} ${pad2(d.getHours())}:${pad2(d.getMinutes())}`;
}

/** 把消息列表按时间分块，块之间插入分隔条 */
type ChatItem =
  | { kind: 'divider'; id: string; label: string }
  | { kind: 'msg'; msg: ConversationMessage };

function groupMessages(messages: ConversationMessage[]): ChatItem[] {
  const items: ChatItem[] = [];
  let prevTs = 0;
  messages.forEach((m, idx) => {
    const isFirst = idx === 0;
    const gapTooLarge = m.timestamp - prevTs > DIVIDER_GAP_MS;
    if (isFirst || gapTooLarge) {
      items.push({
        kind: 'divider',
        id: `divider-${m.id}`,
        label: formatDivider(m.timestamp),
      });
    }
    items.push({ kind: 'msg', msg: m });
    prevTs = m.timestamp;
  });
  return items;
}

/**
 * 微信式对话气泡区。
 * - sign  消息靠右（听障使用人方）青绿色气泡
 * - speech 消息靠左（健听者方）灰白气泡
 * - system 消息居中提示
 * - 跨 5 分钟自动插入时间分隔条
 * - 长消息（>80 字符）支持点击展开/收起
 * - pending=true 的气泡显示三点呼吸动画
 */
export function ChatBubbles({ messages }: Props) {
  const scrollRef = useRef<HTMLDivElement>(null);

  // 自动滚动到底部
  useEffect(() => {
    const el = scrollRef.current;
    if (el) el.scrollTo({ top: el.scrollHeight, behavior: 'smooth' });
  }, [messages.length]);

  const items = useMemo(() => groupMessages(messages), [messages]);
  const empty = messages.length === 0;

  return (
    <div className="chat-bubbles" ref={scrollRef}>
      {empty && <EmptyHint />}
      {items.map((it) =>
        it.kind === 'divider' ? (
          <DateDivider key={it.id} label={it.label} />
        ) : (
          <Bubble key={it.msg.id} msg={it.msg} />
        ),
      )}
    </div>
  );
}

function EmptyHint() {
  return (
    <div className="chat-empty">
      <div className="chat-empty-icon">💬</div>
      <div className="chat-empty-title">等待对话开始</div>
      <div className="chat-empty-sub">
        手套手势 → 转写为文本气泡（右） · 麦克风 ASR → 显示文本气泡（左）
      </div>
    </div>
  );
}

function DateDivider({ label }: { label: string }) {
  return (
    <div className="chat-divider">
      <span className="chat-divider-label">{label}</span>
    </div>
  );
}

function Bubble({ msg }: { msg: ConversationMessage }) {
  const time = useMemo(() => formatTime(msg.timestamp), [msg.timestamp]);
  const [expanded, setExpanded] = useState(false);

  if (msg.role === 'system') {
    return (
      <div className="bubble-row bubble-row-system">
        <div className="bubble-system">
          <span className="bubble-system-dot" />
          {msg.text}
        </div>
      </div>
    );
  }

  const isSign = msg.role === 'sign';
  const isPending = !!msg.pending;
  const tooLong = msg.text.length > FOLD_THRESHOLD;
  const displayText =
    !tooLong || expanded ? msg.text : msg.text.slice(0, FOLD_THRESHOLD) + '…';

  return (
    <div className={`bubble-row ${isSign ? 'bubble-row-right' : 'bubble-row-left'}`}>
      {!isSign && <Avatar role="speech" />}
      <div className="bubble-col">
        <div className="bubble-meta">
          <span className="bubble-name">
            {isSign ? '我（手语）' : '对方（语音）'}
          </span>
          <span className="bubble-time">{time}</span>
          {!isPending && msg.confidence != null && (
            <span className="bubble-conf">
              {Math.round(msg.confidence * 100)}%
            </span>
          )}
          {!isPending && msg.latencyMs != null && (
            <span className="bubble-latency">ASR {msg.latencyMs}ms</span>
          )}
          {isPending && <span className="bubble-pending-tag">识别中</span>}
        </div>
        <div
          className={`bubble bubble-${msg.role}${isPending ? ' bubble-pending' : ''}`}
        >
          {isPending ? <TypingDots /> : displayText}
          {!isPending && tooLong && (
            <button
              type="button"
              className="bubble-fold-toggle"
              onClick={() => setExpanded((v) => !v)}
            >
              {expanded ? '收起' : '展开全部'}
            </button>
          )}
        </div>
      </div>
      {isSign && <Avatar role="sign" />}
    </div>
  );
}

function TypingDots() {
  return (
    <span className="bubble-typing">
      <span className="bubble-typing-dot" />
      <span className="bubble-typing-dot" />
      <span className="bubble-typing-dot" />
    </span>
  );
}

function Avatar({ role }: { role: 'sign' | 'speech' }) {
  const isSign = role === 'sign';
  return (
    <div className={`avatar avatar-${role}`}>
      {isSign ? (
        <svg viewBox="0 0 24 24" width="22" height="22">
          {/* 手语：手 icon */}
          <path
            fill="currentColor"
            d="M12 2c-.6 0-1 .4-1 1v8h-1V5c0-.6-.4-1-1-1s-1 .4-1 1v8H7V7c0-.6-.4-1-1-1s-1 .4-1 1v9c0 3.3 2.7 6 6 6h2c3.3 0 6-2.7 6-6V8c0-.6-.4-1-1-1s-1 .4-1 1v3h-1V3c0-.6-.4-1-1-1s-1 .4-1 1v8h-1V3c0-.6-.4-1-1-1z"
          />
        </svg>
      ) : (
        <svg viewBox="0 0 24 24" width="22" height="22">
          {/* 语音：麦克风 icon */}
          <path
            fill="currentColor"
            d="M12 14a3 3 0 0 0 3-3V5a3 3 0 0 0-6 0v6a3 3 0 0 0 3 3zm5-3a5 5 0 0 1-10 0H5a7 7 0 0 0 6 6.9V21h2v-3.1A7 7 0 0 0 19 11h-2z"
          />
        </svg>
      )}
    </div>
  );
}
