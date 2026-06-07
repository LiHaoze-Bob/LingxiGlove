# LingxiGlove 灵犀手套 - 项目开发文档

## 1. 项目背景与目标

### 1.1 项目简介

**LingxiGlove（灵犀手套）** 是一款基于 Arduino Nano ESP32-S3 的智能手语翻译手套，目标是将聋人手语手势实时转换为语音播报，打破聋人与听人之间的沟通壁垒。

本项目面向 **2026年全国大学生物联网设计竞赛（Espressif赛道）** 开发。

### 1.2 核心功能

1. **手势采集**：通过 MPU6050（6轴惯性传感器）+ 5路弯曲传感器采集手部姿态数据
2. **手势识别**：利用 Edge Impulse 训练的机器学习模型识别手语手势
3. **文本转换**：将识别结果映射为对应的中文文本
4. **语音合成**：调用云端 TTS（阿里 Qwen-TTS）将文本转为语音
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
| 云端 TTS | 阿里 Qwen-TTS (DashScope) |
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
│  gesture_arbitrator.h/cpp               │
│  手势仲裁层（单手/双手统一决策）          │
├─────────────────────────────────────────┤
│  wifi_manager.h/cpp  │  http_client.h/cpp  │  llm_client.h/cpp  │
│  WiFi连接管理         │  HTTP请求封装        │  LLM对话接口       │
├─────────────────────────────────────────┤
│         tts_player.h/cpp                │
│    I2S音频初始化 + Qwen-TTS + LittleFS缓存  │
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
  motion_detector (动作/静止门控)
         │
         ▼
  gesture_recognizer (单手/双手分别识别手势)
         │  GestureCandidate{source, text, confidence}
         ▼
  gesture_arbitrator (统一决策：双手优先、确认窗口、冷却)
         │  ArbitratedGesture{should_announce, text}
         ▼
  tts_player.speak(text) ──→ Qwen-TTS (DashScope)
         │                           │
         ▼                           ▼
    I2S 播放  ←──────  LittleFS 缓存 / 云端 WAV
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

- [x] `sensor_manager` — MPU6050 初始化与数据读取（Wire 手动驱动 + 姿态解算 + 5 路弯曲传感器 ADC 采集，由 `ENABLE_FLEX_SENSORS` 条件编译控制）
- [x] `motion_detector` — 动作/静止门控（加速度方差 + 陀螺仪模长双阈值迟滞状态机）
- [x] `calibration` — IMU 零偏 + Flex 量程个体校准，NVS 持久化，启动自动加载
- [x] `gesture_recognizer` — 规则识别器：RuleBasedRecognizer（俯仰/横滚 + 500ms 防抖）+ BimanualRuleRecognizer（双手协同规则）
- [x] `gesture_arbitrator` — 手势仲裁层（方案C）：单手/双手统一决策，双手优先抑制单手，确认窗口 200ms，冷却 2s
- [x] `esp_now_sync` — 双手 ESP-NOW 同步（HandFrame 30B，MASTER/SLAVE 角色）
- [x] `nvs_config` — 运行时角色/对端 MAC/WiFi 凭证持久化（NVS）
- [x] `wifi_manager` — WiFi 连接与断线重连
- [x] `http_client` — HTTPS GET/POST 封装 + SSE 流式 + URL 编码
- [x] `llm_client` — 阿里通义千问 / 百度 ERNIE 双提供商支持；自然句改写
- [x] `tts_player` — Qwen-TTS 云端合成 + LittleFS 缓存 + I2S 播放 + 离线 PCM 回退
- [x] `local_tts_fallback` — TTS 失败时按 label 匹配离线 PCM；空表降级蜂鸣
- [x] `offline_voice_pcm` — 离线语音 PCM 表（由 `tools/gen_offline_voice_pcm.py` 生成）
- [x] `accuracy_test` — 现场识别准确率统计（混淆矩阵 + LittleFS CSV 日志 + `test` 串口命令族）
- [x] `config.h` — 全局配置中心（特性开关、阈值、引脚定义）
- [x] `LingxiGlove_Main.ino` — 主循环：采集 → 门控 → 识别 → 仲裁 → TTS 播报（含 WiFi 守护 + 串口命令分发）

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
   - `WebSockets` by Markus Sattler（端侧 WS server；仅当 `config.h::ENABLE_WS_SERVER=1` 时需要）

### 6.2 API 配置

首次使用需从模板创建密钥文件：

```bash
cd src/LingxiGlove_Main
cp secrets.example.h secrets.h
```

编辑 `secrets.h`，填入以下内容：

```cpp
#define WIFI_SSID       "你的WiFi名称"      // 仅支持 2.4GHz（ESP32-S3 不支持 5GHz）
#define WIFI_PASSWORD   "你的WiFi密码"
#define QWEN_API_KEY    "sk-xxxxxxxxxxxxxxxx"  // 阿里 DashScope API Key
```

> `secrets.h` 已被 `.gitignore` 忽略，**禁止提交到仓库**。WiFi 凭证也可通过串口 `wifi` 命令运行时设置（写入 NVS，优先级高于 `secrets.h`）。

---

## 7. 调试与排错

### 7.1 常见问题

| 现象 | 原因 | 解决 |
|------|------|------|
| 插入扩展板后 USB 端口消失 | RST 引脚冲突 | RST 引脚不插扩展板，悬空 |
| I2S 播放无声 | 接线错误或喇叭正负接反 | 检查 BCLK/LRC/DIN 接线，喇叭不分正负 |
| TTS 返回错误 | API Key 无效或额度用尽 | 检查 secrets.h 中的 QWEN_API_KEY |
| MPU6050 读取失败 | I2C 地址错误或接线松动 | 确认 AD0 接 GND（地址0x68），检查 SDA/SCL |
| WiFi 连接超时 | 信号弱或密码错误 | 靠近路由器，检查密码中的特殊字符 |
| flex ADC 读数不随弯曲变化 | IDE Pin Numbering 模式导致 analogRead 读错引脚 | 代码统一用 `A2` 等 A 常量，不硬编码 GPIO 编号（详见 §7.5） |

### 7.2 调试开关

```cpp
#define DEBUG_MODE  1   // 设为1开启详细串口日志，0关闭
```

开启后可通过串口监视器（115200波特率）查看所有模块的运行日志。

### 7.3 串口命令

程序启动后可通过串口监视器（**115200 波特率**）发送命令实时控制设备。命令支持单字符和多字符两种格式。

#### 模式切换

| 命令 | 功能 |
|------|------|
| `r` | 恢复**识别模式**（正常手势识别 + TTS 播报） |
| `c` | 进入**词级数据采集模式**（CSV 输出，用于 Edge Impulse 训练） |
| `f` | 进入**指拼采集模式**（CSV 输出，为未来指拼字母表模型预留） |

#### 校准与设备信息

| 命令 | 功能 |
|------|------|
| `k` | 执行**个体零偏校准**（~3 秒，手套平放静止） |
| `i` 或 `info` | 打印当前设备信息（角色、MAC 地址、WiFi SSID、对端 MAC） |
| `h` 或 `?` | 显示帮助 |

#### TTS 与 LLM

| 命令 | 功能 |
|------|------|
| `t <文本>` | **手动 TTS 播报**：合成并播放指定文本 |
| `l <手势序列>` | **LLM 改写 + TTS**：将手势序列改写为自然句并播报 |

示例：
```
t 你好世界
l 我,吃饭
```

#### ESP-NOW 角色与配对（运行时）

| 命令 | 功能 |
|------|------|
| `role master` | 设为 MASTER 角色，写入 NVS 并重启 |
| `role slave` | 设为 SLAVE 角色，写入 NVS 并重启 |
| `peer AA:BB:CC:DD:EE:FF` | 设置对端 MAC 地址，写入 NVS 并重启 |
| `nvs clear` | 清除所有 NVS 配置（角色 + MAC），恢复编译期默认值并重启 |

> **优先级**：NVS > `build_opt.h` 编译期宏 > `config.h` 默认值。

#### WiFi 配置（运行时）

| 命令 | 功能 |
|------|------|
| `wifi <SSID> <PASSWORD>` | 设置 WiFi 凭据，写入 NVS 并重启 |
| `wifi <SSID>` | 设置无密码的开放网络 |
| `wifi clear` | 清除 NVS WiFi 配置，恢复 `secrets.h` 默认值并重启 |

> **优先级**：NVS 已保存值 > `secrets.h` 编译期宏。

#### WebSocket 推流调试（mic on/off/status）

端侧固件作为 WS server 监听 `ws://<本机IP>:81/ws`（端口/路径见 `config.h::WS_SERVER_PORT/WS_SERVER_PATH`），帧 envelope 与 [LingxiGlove_APP/src/lib/wsProto.ts](file:///Users/kun.li/Code/Lingxi/LingxiGlove_APP/src/lib/wsProto.ts) 完全对齐（`{v, kind, ts, payload}`，`v=WS_PROTO_VERSION=1`）。

为单独验证「ESP32 麦克风 → Web App ASR」链路（不必触发 PTT），新增三条串口命令：

| 命令 | 功能 |
|------|------|
| `mic on` | 启动 INMP441 录音，按 32 ms / 块（512 sample × 16-bit / 16 kHz）广播 `audio_chunk` |
| `mic off` | 停止录音；广播 `final=true` 末块 + `mic_state=idle` |
| `mic status` | 打印 streaming / running / clients / helloed / seq |

**依赖**：
- `config.h::ENABLE_WS_SERVER=1` + `ENABLE_MIC_CAPTURE=1`
- Arduino IDE 已装 `WebSockets by Markus Sattler`（参见 §6.1）
- WiFi 就绪后串口会打印 `[WS] 监听 ws://192.168.x.x:81/ws`

**Web App 对接**：在 `LingxiGlove_APP/.env.local` 设置：

```
NEXT_PUBLIC_WS_URL=ws://<ESP32 IP>:81/ws
```

`mic on` 后浏览器 DevTools Network → WS 应能看到 `hello` / `mic_state` / `audio_chunk` 流入。

**录音超时三层兜底**（防止用户忘按 `mic off` 导致阿里云一句话识别 60s 上限「整段白录」）：

| 层级 | 落点 | 触发条件 | 行为 |
|---|---|---|---|
| Lv1 端侧 watchdog | `config.h::WS_MIC_STREAM_MAX_MS=55000` | 单段 ≥ 55s | 自动 `mic off` + 广播 `final=true` |
| Lv2 UI 提示 | `Dashboard.tsx::REC_WARN_MS/REC_MAX_MS` | 30s/50s | 录音计时器变黄 / 红 + 文案提示 |
| Lv3a APP 滚动切片 | `useGloveSystem.ts::ASR_SLICE_BYTES≈50s` | 单段 ≥ 50s | 提前 finalize + 提交一段，继续接收下一段 |

> 多段被触发时气泡前缀 `[1]` `[2]` ...；单段录音不加前缀，体验无变化。修改 Lv1 时务必同步 Lv3a 的阈值，保持 Lv3a < Lv1。

#### 准确率测试

| 命令 | 功能 |
|------|------|
| `test <id> <count>` | 启动准确率测试会话（如 `test 1 30` 测试"你好"×30 次） |
| `test cancel` | 取消当前测试会话 |
| `test export` | 将所有历史测试 CSV 日志 dump 到串口 |
| `test clear` | 清除所有测试日志 + 重置计数器 |
| `test help` | 打印手势 ID 对照表和使用说明 |

手势 ID 对照：
- **单手**：1=你好, 2=谢谢, 3=再见, 4=是, 5=不
- **双手**：101=加油, 102=一起, 103=我爱你, 104=帮助

采集模式的 CSV 列定义：

```
timestamp_ms, ax, ay, az, gx, gy, gz, pitch, roll
```

当 `ENABLE_FLEX_SENSORS=1` 时，表头与每行追加 5 列：`flex0, flex1, flex2, flex3, flex4`。

### 7.4 弯曲传感器启用流程

> **当前物料说明**：到货的弯曲传感器为**单路模块**（VCC/GND/DO/AO 四引脚），含信号调理板，3.3V/5V 宽压兼容。使用 **AO 口接 ESP32-S3 ADC** 读取模拟电压；DO 口为数字高低电平（阈值由板载电位器调节，精度较低，不推荐用于手势识别）。
>
> **接入策略**：目前已接入 1 路，安装在**左手食指**位置（扩展板 A2 接口）。其余 4 路后续采购补齐。
>
> **供电注意**：VCC 接扩展板 **V 排针**（5V）即可。flex 模块内部有分压电路，5V 供电时 AO 输出最高约 2.24V，低于 ESP32-S3 的 3.3V ADC 上限，安全可用。5V 供电动态范围更大（差值 ~1600 count）优于 3.3V（~960 count）。扩展板 V/G/S 排针的 V 列为 5V，无独立 3.3V 排针。
>
> **注意**：传感器只能向**印字一侧**弯曲，反向弯曲会损坏传感器。安装时确认弯曲方向与手指弯曲方向一致。典型阻值参考：平直约 37 kΩ，弯曲 90° 约 90 kΩ。

默认 `ENABLE_FLEX_SENSORS=0`，`SensorData.flexValid=false`，不做任何 ADC 读取。按以下步骤启用单路弯曲传感器：

1. 将 AO 引脚接至 ESP32-S3 的任意 ADC 引脚（**避开 I2C 占用的 A4/A5**），VCC 接 3.3V，GND 接地。
2. 在 `config.h` 的 `#if ENABLE_FLEX_SENSORS` 代码块中定义引脚宏，**必须用 A 常量**（如 `A2`），不能硬编码 GPIO 编号（详见 §7.5）。暂未接入的 4 路可用占位 A 常量：
   ```cpp
   #define FLEX_PIN_THUMB   A0  // 占位 — 未接线
   #define FLEX_PIN_INDEX   A2  // ✅ 左手食指，已接入
   #define FLEX_PIN_MIDDLE  A3  // 占位 — 未接线
   #define FLEX_PIN_RING    A6  // 占位 — 未接线
   #define FLEX_PIN_PINKY   A7  // 占位 — 未接线
   ```
3. 使用采集模式分别记录手指**完全伸直**与**完全弯曲**时的 ADC 读数，填入 `FLEX_ADC_MIN` / `FLEX_ADC_MAX`。当前实测校准值（VCC=5V）：伸直 raw≈2760（→ `FLEX_ADC_MAX=2800`），弯曲 90° raw≈1180（→ `FLEX_ADC_MIN=1100`）。注意：raw 越小=弯曲越大。
4. 把 `ENABLE_FLEX_SENSORS` 改为 `1`，重新编译上传。

若开关置 1 却漏写任何宏，编译会被 `#error` 拦截，强制避免使用未经校准的假设值。

### 7.5 踩坑记录：Arduino Nano ESP32 的 analogRead 引脚映射

#### 问题现象

弯曲传感器 AO 接扩展板 A2，万用表量 A2 电压随弯曲明显变化，但 `analogRead(3)` 读数始终固定（~535），不随弯曲变化。

#### 根本原因

Arduino Nano ESP32 IDE 的 **Tools → Pin Numbering** 有两种模式：

| 模式 | `analogRead(A2)` 等价于 | 说明 |
|------|------------------------|------|
| By Arduino Nano Pin（D-number） | `analogRead(19)` | ✅ 官方推荐，默认设置 |
| By GPIO Number（legacy） | `analogRead(3)` | 已标记为遗留模式 |

在 D-number 模式下，`analogRead(3)` 读的是 **D3（GPIO6）**，而不是物理 A2 引脚（GPIO3/D19）。硬编码 GPIO 编号会导致读取完全错误的通道。

#### 排查过程

1. 硬编码 `GPIO3`（即 `analogRead(3)`）→ 读到固定值 ~535，不随弯曲变化
2. GPIO 扫描（A2 接 GND）→ GPIO1=0，误判 A2=GPIO1
3. 改用 `analogRead(1)` → 始终读 0（GPIO1 未接 flex）
4. 万用表确认 A2 电压随弯曲变化，但代码读不到 → 发现 **Pin Numbering 模式不一致**
5. 用 Arduino `A2` 常量 → 自动解析为 pin 19，成功读到 flex 数据（raw≈1762）

#### 解决方案

**规则**：所有模拟引脚必须用 **A 常量**（`A0`~`A7`），禁止硬编码数字。

```cpp
// ✅ 正确 — 自动适配任何 Pin Numbering 模式
#define FLEX_PIN_INDEX  A2
analogRead(FLEX_PIN_INDEX);

// ❌ 错误 — 仅在特定模式下有效，切换模式即失效
#define FLEX_PIN_INDEX  3    // 仅 GPIO Number 模式有效
#define FLEX_PIN_INDEX  19   // 仅 D-number 模式有效
```

#### 经验教训

- 万用表量到电压变化但代码读不到，首先怀疑 **analogRead 参数是否指向正确的物理引脚**
- GPIO 扫描中“非零值≠浮空”，有外设输出的引脚也会显示非零值，会干扰判断
- A 常量是 Arduino 抽象层的正确用法，能自动适配 IDE 的引脚映射模式

---

## 8. 文件目录结构

```
Lingxi/                              ← 项目根目录
├── README.md                        ← 仓库入口（首次克隆指引 + 工具链）
├── spec.yaml                        ← 机器可读产品契约（与 SDD_SPEC.md 配套）
├── .gitignore                       ← 忽略 secrets.h / .DS_Store / build 产物
├── doc/
│   ├── SDD_SPEC.md                  ← 系统设计契约（架构、模块边界、测试矩阵）
│   ├── DEVELOPMENT.md               ← 本文档（开发指南）
│   ├── SOLUTION_REVIEW.md           ← 整体方案 Review + 双手技术白皮书
│   ├── DOUBLE_HAND_DESIGN.md        ← 双手协同设计白皮书（ESP-NOW + TDOA）
│   ├── USER_GUIDE.md                ← 用户指南（烧录、配置、串口交互）
│   ├── PERFORMANCE_OPTIMIZATION.md  ← 性能优化记录
│   ├── Edge_Impulse_ESP32_S3_训练指南.md  ← Edge Impulse 模型训练指南
│   └── acoustic_tdoa_simulation_results.md ← TDOA 仿真结果
├── src/
│   └── LingxiGlove_Main/            ← Arduino 主项目
│       ├── LingxiGlove_Main.ino     ← 主程序入口（识别/采集/测试多模式 + 串口命令分发）
│       ├── config.h                 ← 全局配置（特性开关、阈值、引脚定义）
│       ├── build_opt.h              ← 编译期角色选择（-DESPNOW_ROLE=0|1）
│       ├── secrets.example.h        ← 密钥模板（首次使用需 cp 为 secrets.h）
│       ├── sensor_manager.h/.cpp    ← 传感器管理（MPU6050 + 5 路弯曲 ADC；姿态解算）
│       ├── motion_detector.h/.cpp   ← 动作/静止门控（加速度方差 + 陀螺仪双阈值）
│       ├── calibration.h/.cpp       ← 个体校准（IMU 零偏 + Flex 量程，NVS 持久化）
│       ├── gesture_recognizer.h/.cpp ← 手势识别（RuleBased + BimanualRule + 抽象基类）
│       ├── gesture_arbitrator.h/.cpp ← 手势仲裁层（方案C：统一决策 + 双手优先）
│       ├── esp_now_sync.h/.cpp      ← 双手 ESP-NOW 同步（HandFrame 30B，MASTER/SLAVE）
│       ├── nvs_config.h/.cpp        ← 运行时角色/MAC/WiFi 持久化
│       ├── wifi_manager.h/.cpp      ← WiFi 连接 + 断线重连
│       ├── http_client.h/.cpp       ← HTTPS GET/POST + SSE 流式
│       ├── llm_client.h/.cpp        ← LLM 改写（Qwen/百度双提供商）
│       ├── tts_player.h/.cpp        ← Qwen-TTS + LittleFS 缓存 + I2S 播放
│       ├── local_tts_fallback.h/.cpp ← TTS 失败兜底（离线 PCM 匹配）
│       ├── offline_voice_pcm.h/.cpp ← 离线语音 PCM 表
│       ├── accuracy_test.h/.cpp     ← 现场准确率测试（LittleFS CSV + test 命令）
│       └── README.md                ← 源码结构说明
├── src/tests/                       ← Host-side 单元测试（纯 C++11，无 Arduino 依赖）
│   ├── test_motion_detector/        ← 动作门控测试
│   ├── test_calibration_core/       ← 校准算法测试
│   ├── test_local_tts_fallback/     ← 离线 TTS 回退测试
│   ├── test_esp_now_sync/           ← HandFrame 协议测试（-Werror -Wpedantic）
│   ├── test_bimanual_recognizer/    ← 双手识别规则测试
│   ├── test_arbitrator/             ← 手势仲裁层测试
│   ├── test_llm_rewrite/            ← LLM 改写解析测试
│   └── test_tts_parsers/            ← TTS/WAV 解析器测试
├── src/tests/test_acoustic_tdoa/    ← Arduino POC（声学 TDOA，暂缓）
├── src/tests/test_speaker/          ← Arduino 音频自检
├── src/tests/test_mic_capture/      ← INMP441 麦克风硬件验证（含接线/I2S配置/判读黄金标准，见目录 README.md）
└── tools/
    ├── gen_offline_voice_pcm.py     ← 调云端 TTS 生成离线 PCM 表
    ├── gen_chirp_pcm.py             ← 生成 17-19kHz chirp PCM（声学 TDOA 用）
    └── acoustic_tdoa_simulate.py    ← 双手 TDOA 测距 Python 仿真
```

---

## 9. 演示词汇清单

> 面向 **2026 年全国大学生物联网设计竞赛** 现场演示，全部词汇均不依赖声学测距，仅用 MPU6050 姿态角 + 1 路拇指弯曲传感器识别，任何人经短暂练习均可完成。
>
> **传感器说明**：当前仅 Master 手（右手）装有 1 路弯曲传感器，接**拇指**位置。弯曲传感器只能区分"拇指弯曲（握拳状）"与"拇指伸直（张开状）"两种状态，无法单独感知其他四根手指。识别特征列中带 ⚠️ 标注的项依赖 IMU 姿态角主导，弯曲传感器作辅助判断。

### 9.1 单手词汇（5 句）

以下手势仅使用**右手（Master 手）** 完成，左手自然垂放。

| # | 词汇 | 手语动作描述 | 识别特征 | 备注 |
|---|------|-------------|---------|------|
| 1 | **你好** | 右手五指伸直，手掌朝前推出，小幅摆动 | pitch↑ + 拇指伸直（AO 低值） | 张开手掌，拇指伸直，pitch 主导 |
| 2 | **谢谢** | 右手五指并拢，手掌朝下，从胸口向前平推 | pitch↓ + 拇指伸直（AO 低值） | 张开手掌，拇指伸直，pitch 方向与你好相反 |
| 3 | **再见** | 右手五指伸直，手掌侧立，左右摆动 | roll 大幅摆动 + 加速度横向周期变化 | 不依赖弯曲传感器，IMU 动态特征主导 |
| 4 | **明白了** | 右手竖起拇指，其余四指握拳，手掌朝前 ⚠️ | 拇指伸直（AO 低值）+ 其余指握拳 + pitch 居中 | 改为"竖拇指"手势，1 路传感器可区分（拇指伸 vs 握拳整体 IMU 无法区分时用此替代食指手势） |
| 5 | **不** | 右手握拳，手腕保持中立姿态，小幅左右摇摆 | 拇指弯曲（AO 高值）+ 静止或小幅 yaw 摆动 | 握拳使拇指弯，AO 读数高，与其他手势差异明显 |

### 9.2 双手协同词汇（5 句）

以下手势需要**双手同时佩戴传感器手套**，通过 ESP-NOW 同步双手数据后识别。Slave 手（左手）当前无弯曲传感器，识别全部依赖左手 IMU 姿态角与双手同步运动特征。

| # | 词汇 | 手语动作描述 | 识别特征 | 备注 |
|---|------|-------------|---------|------|
| 6 | **加油** | 双手握拳，同时向前抬起再收回，重复 2-3 次 | 双手 pitch 同步上下 + 右手拇指弯曲（AO 高值） | 握拳使右手拇指弯，双手 IMU 同步运动 |
| 7 | **一起** | 双手五指并拢，掌心相对，向前同步推出 | 双手 pitch 同方向↑ + 双手 roll 对称接近 0° | 不依赖弯曲传感器，双手 IMU 对称运动主导 |
| 8 | **帮助** | 右手握拳置于左手掌心，左手掌朝上托举 | 右手拇指弯（AO 高值）+ 左手 pitch 朝上静止 + 右手 pitch 居中 | 非对称姿态，右手握拳 + 左手托举可靠区分 |
| 9 | **我爱你** | 双手交叉置胸口，手掌朝内，静止保持 | 双手 pitch 朝内居中 + 双手 roll 交叉方向相反 + 右手拇指伸直 | 静态手势，保持 500 ms 触发防抖 |
| 10 | **我们走** | 双手握拳，同步交替向前摆臂（模拟行走节律） | 双手 pitch 交替周期性变化（异相）+ 右手拇指弯 | ⚠️ 代码待实现（accuracy_test 暂无此 ID）；动态手势，异相摆臂与同相的"加油"可靠区分 |

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
| 弯曲传感器（单路模块，含信号调理板） | 1 路 | ✅ 已到货，规格已确认 | 接拇指，接入 Master 手套 |
| MAX98357A I2S DAC | 1 | ✅ 已有 | Master 手套语音播报 |
| INMP441 麦克风 | 0 | ❌ 未到 | 声学测距（P4 阶段，可跳过） |

> **弯曲传感器规格**：VCC/GND/DO/AO 四引脚，3.3V/5V 宽压兼容，自带信号调理板（含分压电路），无需外接下拉电阻。AO 口接 ADC 读模拟值（平直约 37 kΩ、弯曲 90° 约 90 kΩ）。只能向**印字一侧**弯曲，反向弯曲会损坏传感器。接入步骤详见 §7.4。

### 10.2 开发阶段规划

#### P1：Slave 手套硬件搭建 + ESP-NOW 双板通信 ✅ 已完成（2026-05-14）

**目标**：让第二块 ESP32-S3 + MPU6050 正常运行，并能通过 ESP-NOW 与 Master 同步数据。

- [x] 将第二块 ESP32-S3 烧录 Slave 角色固件（NVS 运行时配置，串口命令 `role slave` 即可切换）
- [x] Slave 板接入 MPU6050，确认 I2C 读数正常
- [x] 验证双板 ESP-NOW 配对：Master 端串口打印 `[双手] ✅ Slave 已连接！首帧已收到`
- [x] 验证 SLAVE 发帧被 MASTER ACK：`GetEspNowTxCount()` 持续增长
- [x] ESP-NOW 信道同步：SLAVE 先连 WiFi AP 同步信道，解决帧全部丢失问题
- [x] NVS 运行时角色配置：一份固件 + 串口 `role master/slave` 切换，无需重编译
- [x] LED 蓝色指示灯：双方通信成功后板载 LED（D16/GPIO45）亮蓝色

**关键实现细节**：
- **信道同步**：ESP-NOW 要求两端在同一 WiFi 信道。MASTER 连 AP 后信道由 AP 决定，SLAVE 必须连同一 AP 才能在相同信道通信。
- **LED 引脚**：Arduino Nano ESP32-S3 板载 RGB LED 是三个独立 GPIO（低电平有效）：Red=GPIO46(D14), Green=GPIO0(D15), Blue=GPIO45(D16)。注意：GPIO48(LED_BUILTIN) 是 SPI SCK，不是 LED。
- **配对成功提示音**：MASTER 首次收到 Slave 帧时播放两声升调蜂鸣。

#### P2：弯曲传感器接入（左手食指已完成 ✅）

**目标**：将 1 路弯曲传感器纳入手势特征，用于左手食指弯曲检测。

> 传感器已确认为单路模块，自带信号调理板，**无需外接分压电阻**。直接将 AO 引脚接 ESP32-S3 的 ADC 引脚，VCC 接 5V（扩展板 V 排针），GND 接地即可（详见 §7.4）。

- [x] 接线：AO → A2（扩展板 A2 接口，左手食指），VCC → 5V（扩展板 V 排针），GND → GND
- [x] 校准（VCC=5V）：伸直 raw≈2760 → `FLEX_ADC_MAX=2800`，弯曲 90° raw≈1180 → `FLEX_ADC_MIN=1100`
- [x] 启用 `ENABLE_FLEX_SENSORS=1`，引脚宏用 A 常量（`FLEX_PIN_INDEX=A2`），其余 4 路占位
- [x] 验证弯曲传感器读数正常：弯曲时 raw 值单调变化，串口日志输出正确（详见 §7.5 踩坑记录）
- [ ] 更新手势规则（`gesture_recognizer.cpp`）：在姿态角判定上叠加食指弯曲传感器条件
- [ ] 其余 4 路传感器到货后补齐接线和校准

#### P3：双手手势数据采集与模型训练

**目标**：采集真实双手手势数据，训练 Edge Impulse 分类模型替换规则识别器。

- [ ] 在 Edge Impulse Studio 创建双手项目，输入维度：`ax/ay/az/gx/gy/gz + flex×N`（左右手合并一帧）
- [ ] 针对 §9 演示词汇清单逐一采集，每词汇至少 50 组样本（不同速度/角度）
- [ ] 训练 1D-CNN 或 Flatten+Dense 轻量模型，目标板上推理 < 50 ms
- [ ] 导出 Arduino 库，实现 `EdgeImpulseRecognizer` 替换 `RuleBasedRecognizer`

#### P4：声学测距（暂缓，视竞赛需求决定）

- §9 全部 10 句演示词汇**均不依赖**双手间距，P3 完成后即可全功能演示
- 若需要演示"远近"语义手势，再启动 INMP441 采购和 TDOA 方案（详见 `DOUBLE_HAND_DESIGN.md` §4.8）

### 10.3 里程碑进度追踪

| 里程碑 | 验收标准 | 状态 |
|--------|---------|------|
| M1：Slave 固件运行 + ESP-NOW 双板通信 | Master 串口打印 `Slave 已连接`，双板 LED 亮蓝色 | ✅ 已完成 (2026-05-14) |
| M2：弯曲传感器接入 + 校准完成 | 采集模式 CSV 弯曲列稳定，握拳/展开 ADC 差值 > 200 count | 🔥 进行中 |
| M3：全词汇演示可跑通（规则识别器） | §9 的 10 个演示词汇均可触发 TTS 播报，误识率 < 20% | ⏳ 待开始 |
| M4：Edge Impulse 模型上板推理 | 10 个词汇识别准确率 > 90%，推理延迟 < 50 ms | ⏳ 待开始 |

### 10.4 已完成的关键技术攻关

| 日期 | 模块 | 内容 | 详见 |
|------|------|------|------|
| 2026-05-01 | TTS | SSE 流式播放修复（line_buf 溢出 + PCM 字节对齐） | MEMORY.md |
| 2026-05-05 | 内存 | DRAM 溢出修复（PSRAM 迁移 192KB） | MEMORY.md |
| 2026-05-09 | TTS | 非流式架构改造 + LittleFS 缓存加速 | MEMORY.md |
| 2026-05-09 | TTS | 播放杂音/拖音/停顿变音全系列修复 | MEMORY.md |
| 2026-05-11 | ESP-NOW | NVS 运行时角色配置（一份固件双角色） | MEMORY.md |
| 2026-05-14 | ESP-NOW | 信道同步修复 + LED 蓝色指示灯 | commit 4c8c887 |
| 2026-05-20 | 双手识别 | `BimanualRuleRecognizer` 补「帮助」规则（左手托举+右手居中）+ host 单元测试 8 用例 | `gesture_recognizer.cpp` / `tests/test_bimanual_recognizer/` |
| 2026-05-22 | 手势仲裁层 | 实现方案C统一决策层：双手优先抑制单手、确认窗口200ms、冷却2s；host 单元测试 8 用例 24 断言 | `gesture_arbitrator.{h,cpp}` / `tests/test_arbitrator/` |
| 2026-05-20 | 准确率测试 | `accuracy_test` 模块：离线手势识别准确率评测框架，LittleFS CSV 日志，`test` 串口命令族 | `accuracy_test.{h,cpp}` |

---

## 11. 工具规划 — 图形化数据采集与打标桌面应用（Action Items）

> 记录于 2026-05-27。目标：替代当前 CLI（`tools/capture_serial.py` + 手动数字键打标 + 手动跑 `build_dataset.py`）的多步流程，做成跨平台一站式 GUI，把"采集 → 切窗 → 上传 EI"的效率从分钟级压到秒级。

### 11.1 顶层目标

- **平台**：macOS（首发）+ Windows（同步支持）
- **形态**：原生桌面应用，单文件分发（`.dmg` / `.exe`），用户无需装 Python
- **定位**：项目级唯一"训练数据生产线 GUI"，长期维护

### 11.2 必备功能（P0 — 端到端闭环必须有）

| 类别 | 功能 | 备注 |
|------|------|------|
| 串口 | 自动检测串口 + USB 拔插自动重连 | macOS 重启后端口名常变 |
| 串口 | 波特率自适应 + 历史记忆 | — |
| 串口 | 多设备同时连接（左右手两块板子） | 阶段 2 双手训练用 |
| 串口 | 实时帧率 / 丢帧率监测，告警阈值可配 | 丢帧 > 10% 红色提示 |
| 打标 | 键盘热键 0-9 / q/w/e... 扩展到 30+ 类 | 与端侧 `CAPTURE_LABEL_NAMES[]` 同步 |
| 打标 | 词库 GUI 管理（增删改 / 批量重命名） | 双向同步到 `config.h` 与 EI 项目 |
| 打标 | 倒计时模式（按空格 → 3-2-1 → 录 N 帧自动停） | 适合离散动作采集 |
| 打标 | 片段标记模式（先录全程 → 回放拖时间轴打标） | 适合连续动作复盘 |
| 数据 | 会话浏览器：按日期/采集人/手语词聚合 | 单条可回放 / 删除 / 重打标 |
| 数据 | 样本统计仪表盘 + 进度条（每类 X / Y 条） | 低于目标高亮 |
| 导出 | 内嵌切窗，直出 EI CSV / npy / CBOR | 替代 `build_dataset.py` |
| 云端 | Edge Impulse Ingestion API 直传 | 免手动上传 |
| 云端 | API Key 用系统钥匙串安全存储 | macOS Keychain / Win Credential |

### 11.3 强烈推荐（P1 — 显著提升体验）

| 类别 | 功能 |
|------|------|
| 可视化 | 5 路 flex 实时折线图（滚动 10s） |
| 可视化 | IMU 3D 姿态实时显示 + 加速度向量 |
| 可视化 | 类内对比：选 label 叠加显示已采样本均值/方差包络 |
| 数据 | 质量过滤：自动剔除全 0 / 平直无变化 / 异常突刺帧 |
| 数据 | 采集人/松紧度/左右手元数据，写入 CSV 头注释 |
| 云端 | 从 EI 拉项目 label 列表，校验端侧/EI 一致性 |
| 云端 | 一键调 EI API 启动训练 |
| 云端 | 模型回流：自动下载 EI `.zip` 到 `output/ei_lib/` |
| 协作 | 会话 zip 打包导出/导入（含元数据 + 可选 webcam 录像） |
| 协作 | 采集 recipe 脚本化（"10 词 × 30 次"流程保存复用） |

### 11.4 锦上添花（P2）

- 暗色模式、中英文切换
- 数据增强预览（噪声/时间扭曲）
- 崩溃自动落盘 raw 数据到 `output/capture/`，下次启动恢复
- 实时预测：勾选已训练模型 `.zip`，边采边显示模型在线推理结果（用于发现误识别样本）
- 双手协同采集时，左右手数据自动按时间戳对齐

### 11.5 技术栈建议

| 方案 | 优点 | 缺点 | 推荐度 |
|------|------|------|--------|
| **Tauri (Rust + React)** | 包体小（< 10 MB）、原生性能、串口/USB 稳 | Rust 学习曲线 | ⭐⭐⭐⭐⭐ |
| Electron + React | 生态成熟、串口库 `serialport` 完善 | 包体大（>100 MB） | ⭐⭐⭐⭐ |
| Python + PyQt6 | 与现有 `tools/*.py` 复用脚本 | 分发要打 PyInstaller，体积大 | ⭐⭐⭐ |
| Flutter Desktop | UI 漂亮 | 串口生态弱 | ⭐⭐ |

**首选 Tauri**：包体小、跨平台一致、串口可走 Rust `tokio-serial`，前端复用 [LingxiGlove_APP](file:///Users/kun.li/Code/Lingxi/LingxiGlove_APP) 同栈（React + Tailwind）。

### 11.6 与现有工程的边界

| 现有 | 新工具中的归宿 |
|------|----------------|
| [tools/capture_serial.py](file:///Users/kun.li/Code/Lingxi/LingxiGlove/tools/capture_serial.py) | 替代（保留作为 CLI 兜底） |
| [tools/build_dataset.py](file:///Users/kun.li/Code/Lingxi/LingxiGlove/tools/build_dataset.py) | 内嵌为切窗模块（保留 CLI） |
| 端侧 MODE_CAPTURE 协议 | **保持不变**（GUI 只是消费方） |
| `output/capture/session_*/` 目录结构 | **保持不变**（保证回滚兼容） |
| `config.h` 的 `CAPTURE_LABEL_*` | GUI 通过 IDE 之外的脚本同步生成 |

### 11.7 落地路线图

- **M1**：MVP——单串口 + 实时折线图 + 热键打标 + 落盘 raw.csv（替代 capture_serial.py）
- **M2**：内嵌切窗 + 会话浏览器 + 样本统计仪表盘
- **M3**：EI 云端直传 + API Key 安全存储 + 词库 GUI
- **M4**：双设备并行 + IMU 3D 可视化 + 模型回流
- **M5**：实时预测 + recipe 脚本 + webcam 录像

启动节点：阶段 2（5 路 sensor 到货后）启动 M1，避免在 1 路验证阶段过度投入。

### 11.8 M1 MVP 实施（已迁移到独立子项目）

**项目位置**：[../../LingxiGlove_Capture/](file:///Users/kun.li/Code/Lingxi/LingxiGlove_Capture)

2026-05-28 升级为 **4 天冲刺**（原 3 天），新增**双手板并行采集**为 P0 必备能力。

| 文档 | 内容 |
|---|---|
| [LingxiGlove_Capture/README.md](file:///Users/kun.li/Code/Lingxi/LingxiGlove_Capture/README.md) | 项目定位、技术栈、快速上手、双设备架构总览 |
| [LingxiGlove_Capture/PLAN.md](file:///Users/kun.li/Code/Lingxi/LingxiGlove_Capture/PLAN.md) | 4 天逐日任务拆分、验收清单、风险拦截 |

**核心架构升级**（vs 原 §11.8 单设备版）：
- 双串口并行：Rust 端每串口独立 tokio 任务，统一 mpsc 汇聚
- 时钟对齐：以 PC `SystemTime::now()` 为基准，设备 millis 仅作参考
- 打标同步：按键事件 PC 端广播到所有活跃 session，左右手 label 完全同步
- 故障隔离：单设备掉线不影响另一设备继续采集
- 文件独立：每设备 `raw_<alias>.csv`，下游 `build_dataset.py` 兼容性 100%

**键盘交互**（简化）：`Space` 录制 / `Enter` 结束 / `0-9` 切 label / `-` 复位 / `?` 帮助

---

*文档版本: v2.5 | 更新日期: 2026-05-28*
