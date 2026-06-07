'use client';

import { useGloveSystem } from '@/hooks/useGloveSystem';
import { ChatBubbles } from '@/components/ChatBubbles';
import { Dashboard } from '@/components/Dashboard';

export default function Home() {
  const { snapshot, conversation, demoMode, setDemoMode } = useGloveSystem();

  // 顶栏点击切换演示模式：必须 user gesture 内 await，确保 AudioContext 在 click handler 同帧 resume
  const handleToggleDemo = async () => {
    try {
      await setDemoMode(!demoMode);
    } catch (err) {
      console.warn('[page] toggle demoMode failed:', err);
    }
  };

  return (
    <div className="app-shell">
      <header className="top-bar">
        <div className="brand">
          <span className="brand-mark" />
          灵犀手套
          <span className="brand-en">LingxiGlove</span>
        </div>
        <div className="top-bar-meta">
          <button
            type="button"
            onClick={handleToggleDemo}
            className={
              demoMode
                ? 'top-bar-pill top-bar-pill--toggle top-bar-pill--toggle-on'
                : 'top-bar-pill top-bar-pill--toggle'
            }
            title={demoMode ? '点击关闭设备 TTS 同播' : '点击开启设备 TTS 同播'}
          >
            <span
              className={
                demoMode
                  ? 'status-dot status-dot--online'
                  : 'status-dot status-dot--offline'
              }
            />
            演示模式
          </button>
          <span className="top-bar-pill">
            {conversation.length} 条对话
          </span>
          <span className="top-bar-pill">
            <span
              className={
                snapshot.system.connectionStatus === 'connected'
                  ? 'status-dot status-dot--online'
                  : snapshot.system.connectionStatus === 'reconnecting'
                  ? 'status-dot status-dot--warn'
                  : 'status-dot status-dot--offline'
              }
            />
            {snapshot.system.connectionStatus === 'connected'
              ? '在线'
              : snapshot.system.connectionStatus === 'reconnecting'
              ? '重连中'
              : '离线'}
          </span>
        </div>
      </header>

      <main className="main-grid">
        <section className="chat-area">
          <ChatBubbles messages={conversation} />
        </section>
        <Dashboard snapshot={snapshot} />
      </main>
    </div>
  );
}
