# LingxiCapture · 快捷键参考

> 所有快捷键都是「全局键」——只要焦点不在 `input`/`textarea`/`contenteditable` 元素上就生效。
> 这意味着你可以把焦点丢在主窗口任意位置直接按键。

## 录制流转

| 按键        | 动作                                       | 状态机要求                            |
|-------------|--------------------------------------------|---------------------------------------|
| `Space`     | 开始 / 暂停 / 继续 录制                    | idle → recording → paused → recording |
| `Enter`     | 停止录制（弹会话摘要 toast）               | recording 或 paused → idle            |
| `R`         | 暂停时继续录制（等价于 `Space` 在 paused） | paused → recording                    |

会话状态只有 3 种：`idle` / `recording` / `paused`。`Space` 会按当前态做最自然的下一步。

## 打标

| 按键    | 动作                                   |
|---------|----------------------------------------|
| `0`...`9` | 设置当前 label 为该数字（最多 10 类）  |
| `-`     | 回到 unlabeled（label = -1）           |

打标即时生效，下一帧采到的样本就会带新 label。后端 `LabelBroadcaster` 把当前 label 注入每一帧的 `frame.label` 字段，落盘到 `raw_<alias>.csv` 的最后一列。

## 帮助 / 引导

| 按键      | 动作                            |
|-----------|---------------------------------|
| `?`       | 切换快捷键面板                  |
| `Shift+/` | 等价 `?`（中文输入法兼容）      |
| `Esc`     | 关闭任意 overlay（帮助 / 引导 / 关于） |

Header 右侧有 3 个超链接按钮：
- **? 快捷键** —— 同 `?` 键
- **入门** —— 重新打开 5 步引导（永远可点，不会被「已完成」吞掉）
- **关于** —— 版本号 / 作者 / repo / EI Key 状态 / 重新观看引导

## 注意事项

1. **输入框焦点优先**：在 PipelinePanel 的设置卡片里输入路径时，所有快捷键都会让位给浏览器原生行为（不会误触录制）。
2. **首启自动弹引导**：第一次打开 app 会弹 OnboardingWizard。完成或跳过后，`localStorage["lingxi-capture/onboarding-done"] = "1"` 记下来。点击 header 的「入门」可重新观看。
3. **EI Key 安全**：API Key 走 macOS Keychain 存储（`service=lingxi-capture, account=edge-impulse-api-key`），不写入 `localStorage` 也不写入磁盘明文。
