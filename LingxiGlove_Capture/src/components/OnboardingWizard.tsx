/**
 * OnboardingWizard — 首启 5 步引导
 *
 * 触发：
 *  - localStorage["lingxi-capture/onboarding-done"] !== "1" 时第一次自动弹
 *  - 或者用户点击底栏「入门」/ 关于页里的「重新观看」按钮
 *
 * 5 步：
 *  1. 欢迎 + 项目定位（数据采集 → 切窗 → EI 直传）
 *  2. 连接串口（指引上方 ConnectBar）
 *  3. 录制 + 打标（Space / 0-9 / Enter）
 *  4. 数据流水线（PipelinePanel 三块）
 *  5. 完成 + 引导填 EI Key
 */
import { useState } from "react";

const LS_KEY = "lingxi-capture/onboarding-done";

interface OnboardingProps {
  open: boolean;
  onClose: () => void;
}

interface Step {
  title: string;
  body: React.ReactNode;
}

const STEPS: Step[] = [
  {
    title: "👋 欢迎使用 LingxiCapture",
    body: (
      <>
        <p>
          这是 <b>双设备并行采集 + 打标 + 一键上传 Edge Impulse</b> 的桌面工具，
          专为 LingxiGlove 数据闭环而生。
        </p>
        <p className="onb__quote">
          整体流程：连接串口 → 实时绘图 → 按 Space 录制 → 数字键打标 → 一键流水线 → EI Studio 训练。
        </p>
      </>
    ),
  },
  {
    title: "①  连接串口",
    body: (
      <>
        <p>
          顶部有 <code>LEFT</code> / <code>RIGHT</code> 两条连接栏：
        </p>
        <ul className="onb__list">
          <li>选下拉里的 <code>/dev/cu.usbmodem*</code>（Nano ESP32）</li>
          <li>波特率默认 <b>115200</b>（除非你改过端侧）</li>
          <li>「连接」后帧率会出现在右侧统计卡片里</li>
        </ul>
        <p className="onb__hint">单手训练？只连一只也能录，会话子目录会按 alias 命名。</p>
      </>
    ),
  },
  {
    title: "②  录制 + 打标",
    body: (
      <>
        <p>所有操作都是键盘快捷键（在非输入框焦点下）：</p>
        <table className="onb__kbd">
          <tbody>
            <tr><td><kbd>Space</kbd></td><td>开始 / 暂停 / 继续</td></tr>
            <tr><td><kbd>0</kbd>~<kbd>9</kbd></td><td>切到对应 label</td></tr>
            <tr><td><kbd>-</kbd></td><td>回到 unlabeled (-1)</td></tr>
            <tr><td><kbd>Enter</kbd></td><td>停止录制 + 弹摘要</td></tr>
            <tr><td><kbd>?</kbd></td><td>调出快捷键面板</td></tr>
          </tbody>
        </table>
      </>
    ),
  },
  {
    title: "③  数据流水线",
    body: (
      <>
        <p>下方「数据流水线」面板分三块：</p>
        <ol className="onb__list">
          <li><b>会话列表</b>：扫 <code>out_root</code> 下所有 session_*</li>
          <li><b>设置</b>：build_dataset.py 路径 / Python / EI API Key（存 macOS Keychain）</li>
          <li><b>运行</b>：build_dataset → upload，或一键流水线</li>
        </ol>
        <p className="onb__hint">
          所有 build_dataset 与 upload 的 stdout/进度都会实时打到日志区。
        </p>
      </>
    ),
  },
  {
    title: "🚀 准备就绪",
    body: (
      <>
        <p>开干前最后 3 件事：</p>
        <ul className="onb__list">
          <li>下方设置卡片填入 <b>EI API Key</b>（一次性，存 Keychain）</li>
          <li>填好 <code>build_dataset.py</code> 路径与 dataset 输出根</li>
          <li>录第一个 30s 会话，跑「① build_dataset」试水</li>
        </ul>
        <p className="onb__hint">
          想再看一遍？随时点底栏「入门」按钮。
        </p>
      </>
    ),
  },
];

export function OnboardingWizard({ open, onClose }: OnboardingProps) {
  const [step, setStep] = useState(0);

  if (!open) return null;
  const s = STEPS[step];
  const isLast = step === STEPS.length - 1;

  const finish = () => {
    localStorage.setItem(LS_KEY, "1");
    onClose();
  };

  return (
    <div className="overlay" onClick={finish}>
      <div className="overlay__panel onb" onClick={(e) => e.stopPropagation()}>
        <div className="onb__head">
          <span className="onb__counter">{step + 1} / {STEPS.length}</span>
          <button type="button" className="link-btn" onClick={finish}>
            跳过 ✕
          </button>
        </div>
        <h3>{s.title}</h3>
        <div className="onb__body">{s.body}</div>
        <div className="onb__steps">
          {STEPS.map((_, i) => (
            <span
              key={i}
              className={"onb__dot" + (i === step ? " onb__dot--active" : "")}
            />
          ))}
        </div>
        <div className="onb__buttons">
          {step > 0 && (
            <button type="button" className="btn btn--ghost" onClick={() => setStep(step - 1)}>
              ← 上一步
            </button>
          )}
          {!isLast ? (
            <button type="button" className="btn btn--primary" onClick={() => setStep(step + 1)}>
              下一步 →
            </button>
          ) : (
            <button type="button" className="btn btn--primary" onClick={finish}>
              开始使用
            </button>
          )}
        </div>
      </div>
    </div>
  );
}

/** 是否首启（即未完成引导）。供 App.tsx 决定是否自动弹 */
export function isFirstRun(): boolean {
  return localStorage.getItem(LS_KEY) !== "1";
}

/** 强制重新观看（关于页用） */
export function resetOnboarding() {
  localStorage.removeItem(LS_KEY);
}
