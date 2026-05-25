# 灵犀手套 Lingxi

> 基于 ESP32-S3 的智能手语翻译手套 —— 为听障人士打通「手语 → 语音」实时沟通通道。

[![Status](https://img.shields.io/badge/status-MVP-green)]()
[![Board](https://img.shields.io/badge/board-Arduino%20Nano%20ESP32-blue)]()
[![License](https://img.shields.io/badge/license-MIT-lightgrey)]()

---

## 项目简介

灵犀手套通过 MPU6050 IMU + 弯曲传感器 + ESP32-S3 边缘计算，结合云端大语言模型（Qwen LLM / Qwen-TTS）
把听障人士的手语动作实时翻译成自然语音，让听障群体能够更便捷地与外界沟通。

- **硬件**：Arduino Nano ESP32 (ABX00083) · MPU6050 · 5× 弯曲传感器 · MAX98357A I2S 功放
- **算法**：规则识别 (MVP) → 1D CNN on Edge Impulse (V1) → 双手同步（ESP-NOW + 声学 TDOA）
- **云端**：阿里 DashScope — Qwen-LLM 自然句改写 · Qwen-TTS 中文语音合成

详细设计与开发计划见 [`doc/SDD_SPEC.md`](doc/SDD_SPEC.md)（设计契约总纲，配套 [`spec.yaml`](spec.yaml)）、[`doc/DEVELOPMENT.md`](doc/DEVELOPMENT.md)（开发指南）和 [`doc/SOLUTION_REVIEW.md`](doc/SOLUTION_REVIEW.md)（方案 Review）。

---

## 目录结构

```
Lingxi/
├── README.md                        ← 本文件（仓库入口）
├── .gitignore                       ← 忽略 secrets.h / .DS_Store / build 产物
├── spec.yaml                        ← 产品规格契约（机器可读，与 SDD_SPEC.md 配套）
├── doc/                             ← 所有文档
│   ├── images/                      ← 架构图 / 数据流图 (PNG + HTML)
│   ├── SDD_SPEC.md                  ← 设计契约总纲（架构 / 模块 / 阶段 / 测试矩阵）
│   ├── DEVELOPMENT.md               ← 开发指南（接线 / 烧录 / 串口命令）
│   ├── SOLUTION_REVIEW.md           ← 整体方案 Review + 答辩素材
│   ├── DOUBLE_HAND_DESIGN.md        ← 双手手语翻译白皮书
│   ├── PERFORMANCE_OPTIMIZATION.md  ← 性能与资源预算
│   ├── USER_GUIDE.md                ← 用户使用指南
│   ├── Edge_Impulse_ESP32_S3_训练指南.md
│   ├── 架构图_灵犀手套Lingxi.html
│   └── acoustic_tdoa_simulation_results.md + .csv
├── src/
│   ├── LingxiGlove_Main/            ← Arduino 主项目（sketch）
│   │   ├── LingxiGlove_Main.ino     ← 入口
│   │   ├── config.h                 ← 非敏感配置（可提交）
│   │   ├── secrets.example.h        ← 敏感配置模板（可提交）
│   │   ├── secrets.h                ← 真实密钥（❌ 不提交，由 .gitignore 忽略）
│   │   └── ...                      ← 其他模块详见 doc/DEVELOPMENT.md
│   └── tests/                       ← Host 端单测与硬件 POC sketch
└── tools/                           ← Python 辅助脚本
```

---

## 🚀 首次克隆后的必做步骤

### 1. 复制并填写敏感配置

敏感信息（WiFi 密码 / API Key）不会随仓库分发，需要你自己填：

```bash
cd src/LingxiGlove_Main
cp secrets.example.h secrets.h
```

用编辑器打开 `secrets.h`，填入：

- `WIFI_SSID` / `WIFI_PASSWORD` — 你的 2.4 GHz Wi-Fi（ESP32-S3 不支持 5 GHz）
- `QWEN_API_KEY` — 阿里 DashScope 北京主站 API Key，申请地址：<https://bailian.console.aliyun.com/>

> ⚠️ **安全提醒**：`secrets.h` 已在 `.gitignore` 中，绝对不要用 `git add -f` 强制提交。
> 如果不慎泄露，请立即到 DashScope 控制台**禁用并重新生成** Key。

### 2. 安装 Arduino IDE 2.x 与板级支持

1. 下载 Arduino IDE 2.x：<https://www.arduino.cc/en/software>
2. Tools → Board → Boards Manager → 搜索 `Arduino ESP32 Boards` 安装
3. Tools → Board → `Arduino Nano ESP32`
4. Library Manager → 安装 `ArduinoJson 7.x`

### 3. 编译并烧录

用 Arduino IDE 打开 `src/LingxiGlove_Main/LingxiGlove_Main.ino`，
**Verify**（✓）→ **Upload**（→）。

> 如遇 USB 枚举失败：**双击板载 RESET 键（B1）** 进 DFU 模式后再上传。

### 4. 打开串口监视器验证

波特率选 `115200`，看到启动横幅后可用以下串口命令：

| 命令 | 说明 |
|------|------|
| `t <文本>` | 手动触发 Qwen-TTS 播报，验证云端链路 |
| `r` | 切回识别模式 |
| `c` | 进入词级采集（数据收集） |
| `f` | 进入指拼采集 |
| `k` | 个体零偏校准（建议首次使用时跑一次） |
| `h` | 帮助 |

---

## 开发路线

| 阶段 | 目标 | 状态 |
|------|------|------|
| MVP-v1.1 | 规则识别 5 类手势 + Qwen-TTS 云端播报 | ✅ 小闭环跑通 |
| V1.0 | Edge Impulse 1D-CNN 模型端侧推理 + LLM 自然句改写 | 🔄 进行中 |
| V1.5 | 双手同步（ESP-NOW + 声学 TDOA） | 📐 方案确定，待硬件 |
| V2.0 | 手机 App 代理模式 · 离线模型下沉 | 🗓️ 规划中 |

详细路线图见 `doc/DEVELOPMENT.md §10` 与 `doc/SDD_SPEC.md §7`。

---

## 贡献

欢迎 Issue / PR。提交代码前请：

1. 确认 `secrets.h` 未被提交（`git status` 不应出现）
2. 遵循项目的 C++11 嵌入式编码规范（见仓库根 `.cursor/rules/` 或 `.aone_copilot/`）
3. 涉及硬件接线变化需同步更新 `doc/DEVELOPMENT.md`

---

## License

MIT
## License

MIT
