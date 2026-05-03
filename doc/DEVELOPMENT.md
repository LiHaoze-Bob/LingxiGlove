# LingxiGlove 灵犀手套 - 项目开发文档

## 1. 项目背景与目标

### 1.1 项目简介

**LingxiGlove（灵犀手套）** 是一款基于 Arduino Nano ESP32-S3 的智能手语翻译手套，目标是将聋人手语手势实时转换为语音播报，打破聋人与听人之间的沟通壁垒。

本项目面向 **2026年全国大学生物联网设计竞赛（Espressif赛道）** 开发。

### 1.2 核心功能

1. **手势采集**：通过 MPU6050（6轴惯性传感器）+ 5路弯曲传感器采集手部姿态数据
2. **手势识别**：利用 Edge Impulse 训练的机器学习模型识别手语手势
3. **文本转换**：将识别结果映射为对应的中文文本
4. **语音合成**：调用云端 TTS（百度语音合成）将文本转为语音
5. **音频播放**：通过 I2S 接口驱动 MAX98357A DAC 模块播放语音

### 1.3 技术栈

| 层级 | 技术方案 |
|------|----------|
| 主控 | Arduino Nano ESP32-S3 (ABX00083) |
| 扩展板 | Keyestudio KS0564 手势手套扩展板 |
| 传感器 | MPU6050 (I2C) + 5x 弯曲传感器 (ADC) |
| 音频 | MAX98357A I2S DAC + 喇叭 |
| 通信 | WiFi 802.11 b/g/n |
| 云端 LLM | 百度 ERNIE / 阿里通义千问 (DashScope) |
| 云端 TTS | 百度语音合成 API |
| 模型训练 | Edge Impulse Studio |

---

## 2. 硬件清单与接线图

### 2.1 硬件清单

| 模块 | 型号/规格 | 数量 | 接口 |
|------|-----------|------|------|
| 主控板 | Arduino Nano ESP32-S3 (ABX00083) | 1 | - |
| 扩展板 | Keyestudio KS0564 手势手套扩展板 | 1 | 排针直插 |
| 惯性传感器 | MPU6050 | 1 | I2C |
| 弯曲传感器 | 电阻式弯曲传感器 | 5 | ADC |
| 音频DAC | MAX98357A I2S 功放模块 | 1 | I2S |
| 扬声器 | 4Ω 3W 小喇叭 | 1 | 模拟 |

### 2.2 关键接线说明

**重要警告**：ESP32-S3 的 RST 引脚不能插入 KS0564 扩展板，否则会导致 USB 端口冲突消失。RST 引脚请悬空处理。

**扩展板右侧红色区域 S/V/G 三列排针定义**（从内侧到外侧）：
- S = 信号（最内侧）
- V = 电源 VCC（中间）
- G = 地线 GND（最外侧）

#### I2S 音频接线（接 MAX98357A）

| MAX98357A | ESP32-S3 引脚 | 扩展板 S 列位置 |
|-----------|---------------|-----------------|
| BCLK      | GPIO7 (D4)    | S列第1个        |
| LRC (WS)  | GPIO8 (D5)    | S列第2个        |
| DIN       | GPIO9 (D6)    | S列第3个        |
| GND       | GND           | G列对应位置     |
| VIN       | 3.3V/5V       | V列对应位置     |

#### MPU6050 接线（I2C）

| MPU6050 | ESP32-S3 引脚 | 说明 |
|---------|---------------|------|
| VCC     | 3.3V          | - |
| GND     | GND           | - |
| SDA     | GPIO11 (A4)   | I2C 数据线 |
| SCL     | GPIO12 (A5)   | I2C 时钟线 |
| AD0     | GND           | I2C 地址 = 0x68 |

#### 弯曲传感器接线（预留，传感器到货后接入）

接扩展板左侧白色区域的 A0-A4 模拟输入口，通过分压电路读取电阻变化。

---

## 3. 系统软件架构

### 3.1 模块划分

```
┌─────────────────────────────────────────┐
│         LingxiGlove_Main.ino            │
│    （主循环：采集→识别→播报）            │
├─────────────────────────────────────────┤
│  sensor_manager.h/cpp   │  gesture_recognizer.h/cpp  │
│  传感器数据采集          │  手势识别引擎（可替换）      │
├─────────────────────────────────────────┤
│  wifi_manager.h/cpp  │  http_client.h/cpp  │  llm_client.h/cpp  │
│  WiFi连接管理         │  HTTP请求封装        │  LLM对话接口       │
├─────────────────────────────────────────┤
│         tts_player.h/cpp                │
│    I2S音频初始化 + 百度TTS流式播放       │
├─────────────────────────────────────────┤
│              config.h                   │
│    全局配置：WiFi/API/I2S引脚/调试开关   │
└─────────────────────────────────────────┘
```

### 3.2 数据流

```
MPU6050 + 弯曲传感器
         │
         ▼
  sensor_manager (读取原始数据)
         │
         ▼
  gesture_recognizer (识别手势)
         │
         ▼
  文本结果 (如 "你好")
         │
         ▼
  tts_player.speak(text) ──→ 百度TTS API
         │                           │
         ▼                           ▼
    I2S 流式播放  ←───────  PCM-16k 音频流
```

### 3.3 扩展性设计

`GestureRecognizer` 采用抽象类设计，MVP阶段使用 `RuleBasedRecognizer`（基于MPU6050姿态角的硬编码规则），全功能阶段可无缝替换为 `EdgeImpulseRecognizer`（调用 Edge Impulse C++ 库运行推理）。

```cpp
class GestureRecognizer {
public:
    virtual bool init() = 0;
    virtual GestureResult recognize(const SensorData& data) = 0;
    virtual const char* getName() const = 0;
};
```

---

## 4. MVP阶段任务拆解

**目标**：弯曲传感器未到货期间，仅基于 MPU6050 验证完整软件链路。

### 4.1 已实现模块（当前状态）

- [x] `wifi_manager` — WiFi 连接与断线重连
- [x] `http_client` — HTTP GET/POST 封装 + URL 编码
- [x] `llm_client` — 百度 ERNIE / 阿里通义千问 双提供商支持
- [x] `tts_player` — I2S 初始化 + 百度 TTS 流式播放
- [x] `config.h` — 全局配置中心
- [x] `sensor_manager` — MPU6050 初始化与数据读取（Wire 手动驱动 + 姿态解算 + 5 路弯曲传感器 ADC 采集，由 `ENABLE_FLEX_SENSORS` 条件编译控制）
- [x] `gesture_recognizer` — 规则识别器：基于俯仰角/横滚角判定手势 + 500ms 防抖
- [x] `LingxiGlove_Main.ino` — 主循环：采集 → 识别 → TTS 播报（含 WiFi 守护 + 串口采集模式）

### 4.2 MVP 增量优化项（纯软件迭代，已完成）

- [x] 弯曲传感器 5 路 ADC 采集实现（`ENABLE_FLEX_SENSORS=0/1` 开关；启用路径包含过采样均值、线性归一化、上下限钳位；引脚与校准阈值在启用时由用户在 `config.h` 中必填，缺失即 `#error` 拦截编译，避免任何假设值）
- [x] 串口「采集模式」：命令 `c` 切换，CSV 格式输出供 Edge Impulse 采样；`r` 回到识别模式；`h` 查看命令帮助
- [x] `DEBUG_PRINT/DEBUG_PRINTLN` 改为可变参数宏（兼容 `DEBUG_PRINTLN(whoami, HEX)` 等多参调用）
- [x] 百度 `access_token` 过期时间修复：`expires_in` 异常小值时回退 30 分钟有效期，避免 `uint32` 回绕导致长期复用空 token
- [x] TTS 流式播放加入总超时（15s）与空读超时（3s）保护
- [x] `playTestTone()` 频率/时长边界校验，避免 `freq=0` 时除零死循环

> 全功能阶段的 Edge Impulse 识别器将在模型训练、导出 Arduino 库、并确定好「窗口长度/采样率/特征通道/标签映射」之后再新增，届时整体接入（不预留任何空壳类或 TODO 占位）。

### 4.3 MVP 手势定义（基于 MPU6050 姿态角）

| 手势 | 判定规则 | 播报文本 |
|------|----------|----------|
| 手掌朝上 | pitch > 45° | "你好" |
| 手掌朝下 | pitch < -45° | "谢谢" |
| 手掌左倾 | roll > 45° | "再见" |
| 手掌右倾 | roll < -45° | "是" |
| 竖直握拳 | abs(pitch) < 20° && abs(roll) < 20° | "不" |

**防抖机制**：同一手势需持续 500ms 才触发识别，识别后冷却 2 秒防止重复播报。

### 4.4 MVP 主循环逻辑

```
setup():
  1. 初始化串口
  2. 初始化传感器 (MPU6050)
  3. 初始化手势识别器
  4. 初始化 I2S 音频
  5. 连接 WiFi
  6. 初始化 LLM（可选，MVP阶段可跳过LLM，直接文本→TTS）

loop():
  1. 读取传感器数据
  2. 执行手势识别
  3. 如果检测到新手势且置信度足够:
       a. 在串口打印识别结果
       b. 调用 speak(text) 播报语音
  4. 检查 WiFi 连接状态
  5. 延迟 50ms（控制采样频率约20Hz）
```

---

## 5. 全功能阶段任务拆解

### 5.1 硬件扩展

- [ ] 接入 5 路弯曲传感器（ADC 读取）
- [ ] 传感器数据采集与标注流程
- [ ] 电池供电方案（锂电池 + 充电模块）

### 5.2 模型训练与部署

- [ ] 在 Edge Impulse Studio 创建项目
- [ ] 采集多组手语手势数据（MPU6050 + 弯曲传感器）
- [ ] 设计特征提取管道（时序窗口 + 频域特征）
- [ ] 训练分类模型（推荐：CNN 或 轻量级 LSTM）
- [ ] 导出 Arduino 库并集成到项目
- [ ] 实现 `EdgeImpulseRecognizer` 替换规则识别器

### 5.3 软件优化

- [ ] 添加 OTA 固件升级支持
- [ ] 本地语音缓存（常用语句预生成 PCM 存于 Flash）
- [ ] 低功耗模式（手势检测间歇唤醒）
- [ ] 多语言支持（中英切换）
- [ ] LLM 上下文对话（结合手势 + 自然语言）

---

## 6. 开发环境配置

### 6.1 Arduino IDE 设置

1. **开发板选择**：工具 → 开发板 → Arduino Nano ESP32
2. **端口选择**：工具 → 端口 → 选择对应 COM/TTY 端口
3. **需要安装的库**：
   - `ArduinoJson` by Benoit Blanchon（JSON解析）
   - `MPU6050` by Electronic Cats 或直接使用 `Wire.h` 手动驱动

### 6.2 API 配置

编辑 `config.h`，填入以下内容：

```cpp
#define WIFI_SSID       "你的WiFi名称"
#define WIFI_PASSWORD   "你的WiFi密码"

// 选择 LLM 提供商（二选一）
#define LLM_PROVIDER_QWEN
// #define LLM_PROVIDER_BAIDU

// 百度 API（如使用百度LLM或TTS，必须填写）
#define BAIDU_API_KEY       "你的百度API Key"
#define BAIDU_SECRET_KEY    "你的百度Secret Key"

// 阿里 API（如使用通义千问，必须填写）
#define QWEN_API_KEY        "你的DashScope API Key"
```

> 百度 TTS 与百度 LLM 共用同一套 access_token，因此无论使用哪家 LLM，百度 API Key 都必须填写（用于 TTS）。

---

## 7. 调试与排错

### 7.1 常见问题

| 现象 | 原因 | 解决 |
|------|------|------|
| 插入扩展板后 USB 端口消失 | RST 引脚冲突 | RST 引脚不插扩展板，悬空 |
| I2S 播放无声 | 接线错误或喇叭正负接反 | 检查 BCLK/LRC/DIN 接线，喇叭不分正负 |
| TTS 返回错误 | access_token 过期或 API Key 错误 | 检查 config.h 中的百度 API 配置 |
| MPU6050 读取失败 | I2C 地址错误或接线松动 | 确认 AD0 接 GND（地址0x68），检查 SDA/SCL |
| WiFi 连接超时 | 信号弱或密码错误 | 靠近路由器，检查密码中的特殊字符 |

### 7.2 调试开关

```cpp
#define DEBUG_MODE  1   // 设为1开启详细串口日志，0关闭
```

开启后可通过串口监视器（115200波特率）查看所有模块的运行日志。

### 7.3 串口命令

程序启动后可通过串口监视器（115200 波特率）发送单字符命令实时切换运行模式：

| 命令 | 功能 |
|------|------|
| `c` / `C` | 进入**采集模式**：暂停识别与 TTS，按 `SENSOR_READ_INTERVAL` 周期以 CSV 格式输出每帧传感器数据，用于 Edge Impulse 训练集采集 |
| `r` / `R` | 回到**识别模式**：恢复手势识别与 TTS 播报 |
| `h` / `H` / `?` | 打印命令帮助 |

采集模式的 CSV 列定义：

```
timestamp_ms, ax, ay, az, gx, gy, gz, pitch, roll
```

当 `ENABLE_FLEX_SENSORS=1` 时，表头与每行追加 5 列：`flex0, flex1, flex2, flex3, flex4`。

### 7.4 弯曲传感器启用流程

默认 `ENABLE_FLEX_SENSORS=0`，`SensorData.flexValid=false`，不做任何 ADC 读取。硬件到货后按以下步骤启用：

1. 按实际接线在 `config.h` 的 `#if ENABLE_FLEX_SENSORS` 代码块**上方**定义 5 个引脚宏（避开 I2C 占用的 A4/A5）：
   ```cpp
   #define FLEX_PIN_THUMB   1   // 按实际接线
   #define FLEX_PIN_INDEX   2
   #define FLEX_PIN_MIDDLE  3
   #define FLEX_PIN_RING    4
   #define FLEX_PIN_PINKY   13  // A6
   ```
2. 使用采集模式分别记录每根手指**完全伸直**与**完全弯曲**时的 ADC 读数，填入 `FLEX_ADC_MIN` / `FLEX_ADC_MAX`。
3. 把 `ENABLE_FLEX_SENSORS` 改为 `1`，重新编译上传。

若开关置 1 却漏写任何宏，编译会被 `#error` 拦截，强制避免使用未经校准的假设值。

---

## 8. 文件目录结构

```
SignLingua/                          ← 项目根目录
├── README.md                        ← 仓库入口（首次克隆指引 + 工具链）
├── .gitignore                       ← 忽略 secrets.h / .DS_Store / build 产物
├── doc/
│   ├── DEVELOPMENT.md               ← 本文档（开发指南）
│   ├── SOLUTION_REVIEW.md           ← 整体方案 Review + 双手手语翻译白皮书前置
│   └── acoustic_tdoa_simulation_results.{md,csv}  ← D1 仿真产物（由下方脚本生成）
├── src/
│   └── LingxiGlove_Main/            ← Arduino 主项目
│       ├── LingxiGlove_Main.ino     ← 主程序入口（识别/采集/FingerSpelling 三模式 + 串口命令）
│       ├── config.h                 ← 全局配置（含 ENABLE_FLEX_SENSORS / ENABLE_ESPNOW_SYNC 开关）
│       ├── sensor_manager.h/.cpp    ← 传感器管理（MPU6050 + 5 路弯曲传感器 ADC；支持校准注入）
│       ├── gesture_recognizer.h/.cpp ← 手势识别引擎（RuleBased + 抽象基类）
│       ├── motion_detector.h/.cpp   ← 动作/静止门控（加速度方差 + 陀螺仪模长双阈值）
│       ├── calibration.h/.cpp       ← 个体校准（IMU 零偏 + Flex 量程，NVS 持久化）
│       ├── offline_voice_pcm.h/.cpp ← 离线语音 PCM 表（空表时自动降级为蜂鸣）
│       ├── local_tts_fallback.h/.cpp ← TTS 失败兜底（从 PCM 表匹配 label 播放）
│       ├── esp_now_sync.h/.cpp      ← 双手 ESP-NOW 同步（HandFrame，MASTER/SLAVE 角色）
│       ├── wifi_manager.h/.cpp      ← WiFi 连接
│       ├── http_client.h/.cpp       ← HTTP 请求
│       ├── llm_client.h/.cpp        ← LLM 接口
│       ├── tts_player.h/.cpp        ← TTS 语音播放（支持 PlayPcmInt16 动态采样率）
│       └── README.md                ← 源码结构说明
├── src/tests/                       ← Host-side 单元测试（纯 C++，不依赖 Arduino）
│   ├── test_motion_detector/        ← 16/16 通过
│   ├── test_calibration_core/       ← 24/24 通过
│   ├── test_local_tts_fallback/     ← 20/20 通过
│   └── test_esp_now_sync/           ← 24/24 通过（-Werror -Wpedantic）
├── test_blink/                      ← 板载 LED 自检
├── test_mpu6050/                    ← MPU6050 读数自检
└── tools/
    ├── gen_offline_voice_pcm.py     ← 调百度 TTS 生成离线 PCM 表
    └── acoustic_tdoa_simulate.py    ← 双手 TDOA 测距 Python 仿真（D 阶段白皮书数据源）
```

---

## 9. 演示词汇清单

> 面向 **2026 年全国大学生物联网设计竞赛** 现场演示，全部词汇均不依赖声学测距，仅用 MPU6050 姿态角 + 弯曲传感器识别，任何人经短暂练习均可完成。

### 9.1 单手词汇（5 句）

以下手势仅使用**右手**完成，左手自然垂放。

| # | 词汇 | 手语动作描述 | 识别特征 | 备注 |
|---|------|-------------|---------|------|
| 1 | **你好** | 右手五指伸直，手掌朝前推出，小幅摆动 | pitch↑ + 弯曲传感器全伸 | 最常见问候，动作直觉 |
| 2 | **谢谢** | 右手五指并拢，手掌朝下，从胸口向前平推 | pitch↓ + 弯曲传感器全伸 | 动作简单，易区分 |
| 3 | **再见** | 右手五指伸直，手掌侧立，左右摆动 | roll > 45° + 加速度摆动 | 与日常挥手一致 |
| 4 | **明白了** | 右手食指伸出，其余弯曲，手掌朝上点头配合 | 食指单独伸出 + pitch↑ | 弯曲传感器可区分食指 |
| 5 | **不** | 右手握拳，手腕保持中立姿态 | 弯曲传感器全弯 + 静止 | 与其他手势差异明显 |

### 9.2 双手协同词汇（5 句）

以下手势需要**双手同时佩戴传感器手套**，通过 ESP-NOW 同步双手数据后识别。

| # | 词汇 | 手语动作描述 | 识别特征 | 备注 |
|---|------|-------------|---------|------|
| 6 | **加油** | 双手握拳，交替或同时向前抬起 | 双手 pitch↑ + 全弯 + 同步运动 | 经典鼓励手势，观众熟悉 |
| 7 | **一起** | 双手食指伸出，并拢靠近后向前指 | 双手食指伸出 + 向心运动 | 方向感清晰 |
| 8 | **帮助** | 右手握拳置于左手掌心，左手托举 | 右手全弯 + 左手掌朝上静止 | 非对称姿态，易区分 |
| 9 | **我爱你** | 双手交叉放胸口，手掌朝内 | 双手 roll 交叉 + pitch 居中 + 全伸 | 情感手势，演示亮点 |
| 10 | **我们走** | 双手握拳，同步向前摆臂（模拟行走） | 双手 pitch 同步周期性变化 + 全弯 | 动态手势，视觉冲击强 |

### 9.3 演示建议

- **演示顺序**：先展示单手词汇（1→5），再展示双手词汇（6→10），层层递进
- **现场话术**：先用手势打出词汇，等待 TTS 播报后，再说明该手势的含义
- **准备动作**：每次手势前回到中立姿态（手自然垂落），避免防抖超时误触发
- **双手词汇前提**：双手 ESP-NOW 配对成功（Master 端串口打印 `[ESPNow] Slave paired`）

---

## 10. 当前物料状态与下一步开发计划

### 10.1 当前物料盘点

| 物料 | 数量 | 状态 | 用途 |
|------|------|------|------|
| Arduino Nano ESP32-S3 (ABX00083) | **2** | ✅ 已到货 | 一块做 Master（右手），一块做 Slave（左手） |
| MPU6050 惯性传感器 | 2 | ✅ 已有 | 双手各 1 个姿态采集 |
| 弯曲传感器 | 1 块（路数待确认） | ✅ 已到货 | 优先接入 Master 手套 |
| MAX98357A I2S DAC | 1 | ✅ 已有 | Master 手套语音播报 |
| INMP441 麦克风 | 0 | ❌ 未到 | 声学测距（P4 阶段，可跳过） |

> **待确认**：弯曲传感器具体路数（1 路 / 5 路？）。若为 1 路，优先接拇指；若为 5 路，按 §7.4 完整接入。

### 10.2 开发阶段规划

#### P1：Slave 手套硬件搭建（当前优先级 🔥）

**目标**：让第二块 ESP32-S3 + MPU6050 正常运行，并能通过 ESP-NOW 与 Master 同步数据。

- [ ] 将第二块 ESP32-S3 烧录 **Slave 角色固件**（在 `config.h` 将 `ESPNOW_ROLE` 切换为 `SLAVE`）
- [ ] Slave 板接入 MPU6050，确认 I2C 读数正常（先烧录 `test_mpu6050` 自检验证）
- [ ] 验证双板 ESP-NOW 配对：Master 端串口应打印 `[ESPNow] Slave paired`，RSSI > -70 dBm
- [ ] 验证双手帧融合：`HandFrame` 左右手数据均有效，`frame.left_valid && frame.right_valid` 同时为真

#### P2：弯曲传感器接入（当前优先级 🔥）

**目标**：让弯曲传感器数据纳入手势特征，区分手指弯曲/伸展状态。

- [ ] 确认传感器路数和分压电路参数（推荐 10 kΩ 下拉电阻 + 3.3 V 供电）
- [ ] 按 §7.4 流程完成校准：记录每根手指完全伸直/弯曲时的 ADC 读数，填入 `config.h`
- [ ] 启用 `ENABLE_FLEX_SENSORS=1`，在采集模式下验证弯曲列数据稳定输出
- [ ] 更新手势规则（`gesture_recognizer.cpp`）：在姿态角判定上叠加弯曲传感器条件，区分上表 9.1 的各手势

#### P3：双手手势数据采集与模型训练

**目标**：采集真实双手手势数据，训练 Edge Impulse 分类模型替换规则识别器。

- [ ] 在 Edge Impulse Studio 创建双手项目，输入维度：`ax/ay/az/gx/gy/gz + flex×N`（左右手合并一帧）
- [ ] 针对 §9 演示词汇清单逐一采集，每词汇至少 50 组样本（不同速度/角度）
- [ ] 训练 1D-CNN 或 Flatten+Dense 轻量模型，目标板上推理 < 50 ms
- [ ] 导出 Arduino 库，实现 `EdgeImpulseRecognizer` 替换 `RuleBasedRecognizer`

#### P4：声学测距（暂缓，视竞赛需求决定）

- §9 全部 10 句演示词汇**均不依赖**双手间距，P3 完成后即可全功能演示
- 若需要演示"远近"语义手势，再启动 INMP441 采购和 TDOA 方案（详见 `DOUBLE_HAND_DESIGN.md` §4.8）

### 10.3 近期里程碑

| 里程碑 | 验收标准 |
|--------|---------|
| M1：Slave 固件运行 + ESP-NOW 双板通信 | Master 串口实时打印双手融合帧，延迟 < 30 ms |
| M2：弯曲传感器接入 + 校准完成 | 采集模式 CSV 弯曲列稳定，握拳/展开 ADC 差值 > 200 count |
| M3：全词汇演示可跑通（规则识别器） | §9 的 10 个演示词汇均可触发 TTS 播报，误识率 < 20% |
| M4：Edge Impulse 模型上板推理 | 10 个词汇识别准确率 > 90%，推理延迟 < 50 ms |

---

*文档版本: MVP-v1.2 | 更新日期: 2026-05-01*
