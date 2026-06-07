/**
 * AboutModal — 关于页
 *
 * 显示：版本号、作者、repo 路径、当前 EI Key 是否已配置、重新观看引导按钮
 */
import { useEffect, useState } from "react";
import { hasEiKey } from "../api";
import { resetOnboarding } from "./OnboardingWizard";

interface AboutProps {
  open: boolean;
  onClose: () => void;
  onReplayOnboarding: () => void;
}

const APP_VERSION = "0.1.0";
const REPO_URL = "https://github.com/Haoze-Li/Lingxi";

export function AboutModal({ open, onClose, onReplayOnboarding }: AboutProps) {
  const [eiKeyOk, setEiKeyOk] = useState<boolean | null>(null);

  useEffect(() => {
    if (!open) return;
    hasEiKey()
      .then(setEiKeyOk)
      .catch(() => setEiKeyOk(null));
  }, [open]);

  if (!open) return null;

  const replay = () => {
    resetOnboarding();
    onClose();
    onReplayOnboarding();
  };

  return (
    <div className="overlay" onClick={onClose}>
      <div className="overlay__panel about" onClick={(e) => e.stopPropagation()}>
        <h3>关于 LingxiCapture</h3>
        <table className="about__table">
          <tbody>
            <tr>
              <td>版本</td>
              <td><code>{APP_VERSION}</code></td>
            </tr>
            <tr>
              <td>作者</td>
              <td>Haoze.Li &lt;haoze.li@outlook.com&gt;</td>
            </tr>
            <tr>
              <td>仓库</td>
              <td><code>{REPO_URL}</code></td>
            </tr>
            <tr>
              <td>EI Key</td>
              <td>
                {eiKeyOk === null ? (
                  <span className="about__pill about__pill--unknown">未知</span>
                ) : eiKeyOk ? (
                  <span className="about__pill about__pill--ok">已配置</span>
                ) : (
                  <span className="about__pill about__pill--miss">未配置</span>
                )}
              </td>
            </tr>
            <tr>
              <td>用途</td>
              <td>双设备并行采集 + 打标 + 一键上传 Edge Impulse</td>
            </tr>
          </tbody>
        </table>
        <div className="about__buttons">
          <button type="button" className="btn btn--ghost" onClick={replay}>
            重新观看入门引导
          </button>
          <button type="button" className="btn btn--primary" onClick={onClose}>
            关闭
          </button>
        </div>
      </div>
    </div>
  );
}
