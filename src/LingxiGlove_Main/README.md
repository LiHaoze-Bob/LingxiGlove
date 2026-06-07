# LingxiGlove 源码结构说明

## Arduino 项目组织方式

### 1. 编译机制

Arduino IDE 的编译器会：

- **自动拼接所有 `.ino` 文件**：同一文件夹下所有 `.ino` 文件会按文件名排序后拼接成一个临时文件，然后编译。所以函数定义可以分散在不同 `.ino` 文件中，不需要 `#include`。
- **自动编译 `.cpp` 文件**：同一文件夹下所有 `.cpp` 和 `.c` 文件会被自动编译并链接。
- **`.h` 头文件**：需要通过 `#include` 引用，用于声明函数和类。

### 2. 文件组织建议

本项目的组织方式（推荐）：

```
LingxiGlove_Main/              ← 项目文件夹（必须与主 .ino 同名）
├── LingxiGlove_Main.ino      ← 主入口：setup() + loop()，调用各模块
├── config.h                 ← 所有配置常量（WiFi密码、API Key、运行参数等）
├── sensor_manager.h/.cpp    ← 传感器管理（MPU6050 + 5 路弯曲传感器 ADC，由条件编译开关控制）
├── gesture_recognizer.h/.cpp ← 手势识别引擎（规则识别 / 可替换为ML模型）
├── wifi_manager.h/.cpp      ← WiFi连接模块
├── http_client.h/.cpp       ← 通用HTTP请求封装
├── llm_client.h/.cpp        ← LLM大模型接口（支持百度/阿里）
├── tts_player.h/.cpp        ← Qwen-TTS + LittleFS 缓存 + I2S音频播放
└── README.md                ← 本文档
```

**为什么不用多个 `.ino` 文件？**

虽然Arduino IDE支持多个`.ino`文件自动拼接，但这会导致：
- 代码顺序不可控（按文件名排序）
- 难以管理依赖关系
- 不利于代码复用

**推荐做法**：把功能模块写成 `.h/.cpp` 对，主 `.ino` 文件只负责初始化和调度。

### 3. 需要安装的库

在 Arduino IDE 中安装以下库：

- **ArduinoJson** by Benoit Blanchon（用于JSON解析）
  - 菜单：工具 → 管理库 → 搜索 "ArduinoJson" → 安装
- **WebSockets** by Markus Sattler（端侧 WS server，与 Web App 对接 audio_chunk 推流）
  - 菜单：工具 → 管理库 → 搜索 "WebSockets" → 选 "by Markus Sattler" → 安装
  - 仅当 `config.h::ENABLE_WS_SERVER=1` 时需要；置 0 可彻底跳过依赖

ESP32 的 WiFi 和 HTTPClient 库已内置，无需安装。

### 4. 编译上传步骤

1. 打开 Arduino IDE
2. 文件 → 打开 → 选择 `LingxiGlove_Main/LingxiGlove_Main.ino`
3. 工具 → 开发板 → Arduino Nano ESP32
4. 工具 → 端口 → 选择对应端口
5. 点击上传按钮

### 5. 快速开始

1. 编辑 `config.h`，填入你的 WiFi 密码和 API Key
2. 选择 LLM 提供商（百度或阿里），或关闭 LLM 测试（`ENABLE_LLM_TEST 0`）
3. 上传代码到 ESP32-S3
4. 打开串口监视器（115200波特率）查看运行日志

### 6. MVP 阶段说明（当前版本）

弯曲传感器尚未到货，当前版本仅基于 **MPU6050** 验证完整软件链路：

```
MPU6050 读取 → 姿态角解算 → 规则手势识别 → 文本 → Qwen-TTS → I2S播放
```

支持的手势（通过改变手掌姿态触发）：

| 手掌姿态 | 播报内容 |
|----------|----------|
| 朝上（pitch > 45°） | "你好" |
| 朝下（pitch < -45°） | "谢谢" |
| 左倾（roll > 45°） | "再见" |
| 右倾（roll < -45°） | "是" |
| 竖直/水平（中性） | "不" |

**防抖机制**：同一姿态需持续 500ms 才触发识别，识别后有 2 秒冷却时间防止重复播报。

### 7. 运行时串口命令

程序启动后可通过串口监视器（115200 波特率）发送单字符命令实时切换模式：

- `r`：回到**识别模式**（默认模式，启动后即处于此模式）
- `c`：进入**词级采集模式**，暂停识别与 TTS，以 CSV 格式输出每帧传感器数据（供 Edge Impulse 词级手势训练集采集）
- `f`：进入**指拼采集模式**（Finger Spelling），同样输出 CSV；专门为未来"字母指拼兜底通道"预留接入点。**指拼识别模型尚未训练，该模式当前不进行识别，只做原始数据采集**，避免用词级姿态角规则伪造字母输出
- `h` 或 `?`：显示命令帮助

#### WebSocket 推流调试命令（mic on/off/status）

为方便单独测试 ESP32 ↔ Web App 的 WS 链路（不依赖 PTT 双击触发），提供如下命令：

- `mic on`：开启 INMP441 录音，并按 32 ms / 块（512 sample × 16-bit / 16 kHz）通过 WS 广播 `audio_chunk` 帧
- `mic off`：关闭录音；广播 `final=true` 末块 + `mic_state=idle`
- `mic status`：打印当前推流状态（streaming / running / clients / helloed / seq）

依赖：
- `config.h::ENABLE_MIC_CAPTURE=1` 且 `config.h::ENABLE_WS_SERVER=1`
- WiFi 已连上，串口启动日志会打印 `[WS] 监听 ws://<本机IP>:81/ws`
- Web App 端将 `NEXT_PUBLIC_WS_URL` 配为 `ws://<ESP32 IP>:81/ws` 后即可收到帧

**录音超时保护**：单段录音超过 `WS_MIC_STREAM_MAX_MS`（默认 55s）时，端侧自动触发 `mic off` 并广播 `final=true` 末块——这是为对齐阿里云一句话识别 60s 上限的硬保护。APP 端配合在 50s 时滚动切片提前提交，结果以多段气泡 `[1] [2] ...` 呈现。详见 [doc/DEVELOPMENT.md §7.3 — WebSocket 推流调试](file:///Users/kun.li/Code/Lingxi/LingxiGlove/doc/DEVELOPMENT.md)。

CSV 列：`timestamp_ms, ax, ay, az, gx, gy, gz, pitch, roll`（启用弯曲传感器后追加 `flex0..flex4`）。

词级与指拼两种采集模式共享同一 CSV 列定义；两者的**语义差异（词 vs 字母标签）由上位机采集脚本 / Edge Impulse 项目在录制时区分**。

### 8. 弯曲传感器启用

默认 `ENABLE_FLEX_SENSORS=0`，不做任何 ADC 读取。硬件到货后，在 `config.h` 的 `#if ENABLE_FLEX_SENSORS` 前定义 `FLEX_PIN_THUMB/INDEX/MIDDLE/RING/PINKY` 与校准后的 `FLEX_ADC_MIN/MAX`，再把开关改为 1。漏写任何一项将被 `#error` 拦截编译，避免使用假设值。

### 13. 双手同步接口（ESP-NOW 预埋）

为"双手手语翻译"方案预留底层通信层，当前为**接口预埋**，MVP 行为零变化：

- `config.h::ENABLE_ESPNOW_SYNC`：默认 `0`，编译期不引入 `<esp_now.h>` 依赖
- `esp_now_sync.h/cpp`：
  - `struct HandFrame`（`__attribute__((packed))`）：双手帧的线上格式，
    包含 `master_timestamp_ms / seq_no / ax..gz / flex[FLEX_CHANNEL_COUNT]`，
    字段布局固定且跨 MCU 一致（host 单测里用 `offsetof` 固定验证）
  - `InitEspNowSync(role, peer_mac)`：SLAVE/MASTER 角色初始化，`peer_mac=nullptr`
    时注册广播 peer
  - `SendHandFrame(frame)`：异步入队发送
  - `RegisterHandFrameHandler(handler)`：注册接收回调
  - `GetEspNowRxCount / GetEspNowTxCount / GetEspNowTxFailCount`：自检计数
    （Tx = 已 ACK 成功次数；TxFail = send_cb 报告失败次数；Rx = 收到合法帧次数）
- 角色差异化（`s_role` 真实生效）：
  - MASTER 初始化必须提供明确 `peer_mac`（`nullptr` 直接拒绝）；
    MASTER 的 `SendHandFrame` 也会拦截广播地址，避免污染频段
  - SLAVE 允许 `peer_mac=nullptr`，落到广播 `FF:FF:FF:FF:FF:FF`
- ENABLE_ESPNOW_SYNC=0 下所有接口为 stub：`Init` 返回 `false`、`Send` 返回
  `false`、所有计数恒为 0；**严禁任何假数据/回环模拟**
- 启用方式：
  1. 两只手套分别烧写不同 role（MASTER / SLAVE）
  2. 在 `config.h` 置 `ENABLE_ESPNOW_SYNC=1`
  3. 在主程序 `setup()` 调 `InitEspNowSync(ESPNOW_ROLE_MASTER, slave_mac)`（主手）或
     `InitEspNowSync(ESPNOW_ROLE_SLAVE, master_mac)`（从手）
  4. SLAVE 侧 `loop()` 每次读到 `SensorData` 后打包成 `HandFrame` 调 `SendHandFrame`
  5. MASTER 侧 `RegisterHandFrameHandler` 做双手特征拼接、送识别器

注意：ESP-NOW 与普通 Wi-Fi 共用射频且必须同信道，启用后对在线 TTS / LLM
的带宽 / 延迟影响需实测评估。正式接入方案详见 `doc/DOUBLE_HAND_DESIGN.md`。

### 12. 离线 TTS 兜底 (LocalTtsFallback)

网络抖动 / 百度 Token 失效 / TTS 配额耗尽等情况下，在线播报会失败。为此系统
提供一层**纯查表**的本地 PCM 兜底：

- `offline_voice_pcm.h/cpp`：声明结构体 `OfflinePcmEntry { label, data, sample_count, sample_rate }`
  和常量表 `kOfflinePcmTable[]` + `kOfflinePcmCount`。**默认表为空**——
  严禁塞入任何"占位" / "伪造"数据假装能播报
- `local_tts_fallback.h/cpp`：`PlayOfflineVoice(label)` 在表里精确匹配 label，
  命中则调用 `PlayPcmInt16` 通过 I2S 输出；未命中 / 表空返回 false
- 主循环策略：`speak(text)` 失败 → `PlayOfflineVoice(text)` → 两级都失败
  打印日志，**不做蜂鸣兜底**（避免把"无数据"伪装成"正常播报"）

填充离线数据的方法：运行 `tools/gen_offline_voice_pcm.py`（需填入云端 TTS
API Key），脚本会：
1. 对每个输入 label 调用云端 TTS → 16kHz PCM
2. 生成新的 `offline_voice_pcm.cpp`（不动 `.h`），覆盖原空表
3. 重新编译上传即可生效

I2S 播放通道复用 `tts_player` 的 `PlayPcmInt16(pcm, count, sample_rate)`，
该 API 对指针 / 长度 / 采样率做了严格校验（采样率必须在 [8000, 48000]、
样本数 ≤ 10 秒），越界直接返回 false 不尝试播放。

### 11. 个体校准 (Calibration)

MPU6050 每颗模块都有独立的零偏（厂测合格但非零），不同用户佩戴手套的松紧
也会让弯曲传感器的 ADC 量程漂移。`calibration.h/cpp` 为此提供一套
NVS 持久化的个体化校准方案：

- **串口命令 `k`**：触发交互式校准。仅在识别模式下接受；采集/指拼模式下忽略并提示
- **校准内容**：
  1. **IMU 零偏**（必做）：手套平放 3 秒，采集 accel/gyro 均值作为偏移；`accel_z` 额外扣 1g 重力
  2. **Flex 量程**（仅 `ENABLE_FLEX_SENSORS=1`）：五指伸直 3 秒取 min，握拳 3 秒取 max；max 与 min 差异过小（≤32 ADC 码）视为用户操作不到位，该路不接受
- **持久化**：写入 NVS 命名空间 `lingxi_cal`，带版本号，重启后 `setup()` 自动
  加载并调用 `ApplyCalibration(g_cal)` 把偏移/量程注入 `sensor_manager`
- **落点**：IMU 偏移在 `sensor_manager.cpp::readSensors()` 的物理单位换算之后、
  `computeOrientation()` 之前减除——保证 pitch/roll 也反映校准；Flex 量程通过
  运行时变量 `s_flex_min[]/s_flex_max[]` 覆盖 `config.h` 的默认 `FLEX_ADC_MIN/MAX`
- **未校准行为**：`CalibrationData` 全零、`flags=0`，`sensor_manager` 使用默认值
  （等价于"直出裸数据 + config.h 默认量程"）。**未伪造任何假数据**

版本升级策略：修改 `CalibrationData` 存储布局后需把 `CALIBRATION_NVS_VERSION` +1，
老数据在下次启动时自动作废（视为未校准），避免跨版本 struct 布局漂移导致字段错位。

### 10. 动作/静止门控 (MotionDetector)

手语是动作流，静止姿势不应该被反复识别。主循环在调用识别器前串入了一层
二值门控：

- **STILL → MOVING**：窗口内 |a| 方差 > `MOTION_VAR_ENTER` 或 |gyro| 模长 > `MOTION_GYRO_ENTER`
- **MOVING → STILL**：两个特征都落回 EXIT 阈值之下且连续保持 `MOTION_STILL_HOLD_FRAMES` 帧

STILL 状态下识别器不会被调用，同时 `g_lastAnnouncedGesture` 复位以便下一次
运动起来时允许再次播报同一手势。关闭门控只需在 `config.h` 设
`ENABLE_MOTION_GATING 0`。

默认阈值基于 MPU6050 datasheet 噪声水平推算（见 `config.h` 注释），**真实
佩戴条件下需用采集模式录制静止段/运动段各若干秒后二次调参**。调参可读
串口打印的 `[门控] 状态切换: ... var(|a|)=... |gyro|=...` 获取当场数值。

门控逻辑纯 C++ 实现、不依赖 Arduino API（见 `motion_detector.cpp` 的双遍
均值-方差算法与滞回状态机），可在 PC 上做 host-side 单元测试。

### 9. 全功能阶段（规划）

弯曲传感器到货并完成 Edge Impulse 模型训练后，将新增 `EdgeImpulseRecognizer` 类并在工厂函数中接入——届时会一次性提供完整实现（窗口长度、采样率、特征布局、标签映射均以训练产物为准），当前代码中不预留任何空壳类。
