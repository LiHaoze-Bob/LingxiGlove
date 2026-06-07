/**
 * App — Day 6 主入口
 *
 * 顶层 Tab 切换：
 *   - 数据采集（CaptureTab）：现状采集 / 标注 / 流水线
 *   - 设备校准（CalibrationTab）：双板独立 IMU/Flex 校准 + NVS 持久化
 *
 * 切 Tab 不强制断开任何串口（capture tab 用 left/right alias、calibration tab 用
 * CAL_LEFT/CAL_RIGHT，alias 不同则后端 SerialTask 完全独立）。
 *
 * 全局快捷键 + Help/Summary overlay。
 */
import { useCallback, useEffect, useState } from "react";
import { CaptureTab } from "./components/CaptureTab";
import { CalibrationTab } from "./components/CalibrationTab";
import { KeyHelp } from "./components/KeyHelp";
import { SummaryToast } from "./components/SummaryToast";
import { OnboardingWizard, isFirstRun } from "./components/OnboardingWizard";
import { AboutModal } from "./components/AboutModal";
import { CountdownOverlay } from "./components/CountdownOverlay";
import { RecordingHUD } from "./components/RecordingHUD";
import { LabelEditor } from "./components/LabelEditor";
import { useCaptureStore } from "./store";
import { useGlobalKeydown } from "./hooks/useGlobalKeydown";
import "./App.css";

interface CaptureMetrics {
  masterConnected: number;
  totalFps: number;
  portCount: number;
}

function App() {
  const [helpOpen, setHelpOpen] = useState(false);
  const [onboardingOpen, setOnboardingOpen] = useState(false);
  const [aboutOpen, setAboutOpen] = useState(false);
  const [labelEditorOpen, setLabelEditorOpen] = useState(false);
  const [metrics, setMetrics] = useState<CaptureMetrics>({
    masterConnected: 0,
    totalFps: 0,
    portCount: 0,
  });

  const activeTab = useCaptureStore((s) => s.activeTab);
  const setActiveTab = useCaptureStore((s) => s.setActiveTab);

  // 校准 Tab 激活时禁用全局录制快捷键，避免误录制
  useGlobalKeydown({
    onToggleHelp: () => setHelpOpen((v) => !v),
    onCloseHelp: () => setHelpOpen(false),
    enabled: activeTab === "capture",
  });

  // 首启自动弹引导
  useEffect(() => {
    if (isFirstRun()) setOnboardingOpen(true);
  }, []);

  const handleMetrics = useCallback((m: CaptureMetrics) => {
    setMetrics(m);
  }, []);

  return (
    <div className="app">
      <header className="app__header">
        <span className="app__title">LingxiCapture</span>

        <nav className="app__tabs" role="tablist">
          <button
            type="button"
            role="tab"
            aria-selected={activeTab === "capture"}
            className={`app__tab ${activeTab === "capture" ? "app__tab--active" : ""}`}
            onClick={() => setActiveTab("capture")}
          >
            数据采集
          </button>
          <button
            type="button"
            role="tab"
            aria-selected={activeTab === "calibration"}
            className={`app__tab ${activeTab === "calibration" ? "app__tab--active" : ""}`}
            onClick={() => setActiveTab("calibration")}
          >
            设备校准 &amp; 配置
          </button>
        </nav>

        <span className="app__hint">
          <button
            type="button"
            className="link-btn"
            onClick={() => setHelpOpen(true)}
          >
            ? 快捷键
          </button>
          {" · "}
          <button
            type="button"
            className="link-btn"
            onClick={() => setLabelEditorOpen(true)}
          >
            Label 编辑
          </button>
          {" · "}
          <button
            type="button"
            className="link-btn"
            onClick={() => setOnboardingOpen(true)}
          >
            入门
          </button>
          {" · "}
          <button
            type="button"
            className="link-btn"
            onClick={() => setAboutOpen(true)}
          >
            关于
          </button>
        </span>
      </header>

      <main className="app__main">
        {/*
          两个 Tab 始终挂载（display 切换），保持事件订阅 + 串口连接不被打断。
          切 Tab 仅是改变可见性。
        */}
        <div style={{ display: activeTab === "capture" ? "block" : "none" }}>
          <CaptureTab onMetrics={handleMetrics} />
        </div>
        <div
          style={{ display: activeTab === "calibration" ? "block" : "none" }}
        >
          <CalibrationTab />
        </div>
      </main>

      <footer className="app__footer">
        {activeTab === "capture" ? (
          <>
            <span>已连接 {metrics.masterConnected} / 1</span>
            <span>总 {metrics.totalFps.toFixed(1)} fps</span>
            <span className="app__footer-hint">
              快捷键：<kbd>Space</kbd> 倒计时录制 · <kbd>0-9</kbd> 打标 ·{" "}
              <kbd>Enter</kbd> 停止 · <kbd>Esc</kbd> 取消 · <kbd>?</kbd> 帮助
            </span>
            <span style={{ marginLeft: "auto" }}>
              ports: {metrics.portCount}
            </span>
          </>
        ) : (
          <>
            <span>设备校准 &amp; 配置 · 双板独立</span>
            <span className="app__footer-hint">
              校准 / 角色 / MAC / WiFi 均写入板载 NVS（部分操作会触发设备自动重启）
            </span>
          </>
        )}
      </footer>

      <KeyHelp open={helpOpen} onClose={() => setHelpOpen(false)} />
      <SummaryToast />
      <CountdownOverlay />
      <RecordingHUD />
      <LabelEditor
        open={labelEditorOpen}
        onClose={() => setLabelEditorOpen(false)}
      />
      <OnboardingWizard
        open={onboardingOpen}
        onClose={() => setOnboardingOpen(false)}
      />
      <AboutModal
        open={aboutOpen}
        onClose={() => setAboutOpen(false)}
        onReplayOnboarding={() => setOnboardingOpen(true)}
      />
    </div>
  );
}

export default App;
