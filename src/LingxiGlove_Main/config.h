// ============================================================
// LingxiGlove 全局配置文件
// ============================================================
// 本文件仅存放"非敏感"的项目配置（引脚、开关、阈值等）。
// 敏感信息（WiFi 密码 / API Key）被拆分到 secrets.h，后者已在
// .gitignore 中忽略，不会提交到仓库。首次克隆请执行：
//     cp secrets.example.h secrets.h
// 并在 secrets.h 中填入真实值。
// ============================================================

#ifndef CONFIG_H
#define CONFIG_H

// 敏感配置：WIFI_SSID / WIFI_PASSWORD / QWEN_API_KEY / BAIDU_API_KEY / BAIDU_SECRET_KEY
// 全部来自 secrets.h；若克隆后未复制 secrets.example.h → secrets.h，编译会在下面的
// #include 处直接失败，提示缺少 secrets.h，从而强制开发者完成密钥配置。
#include "secrets.h"

// ------------------- WiFi 通用参数（非敏感） -------------------
#define WIFI_TIMEOUT_MS 10000                 // WiFi连接超时时间（毫秒）

// ------------------- LLM 提供商选择 -------------------
// 取消注释你想使用的一家，其余保持注释状态
#define LLM_PROVIDER_QWEN
// #define LLM_PROVIDER_BAIDU

// ------------------- 阿里 DashScope Qwen-TTS 配置 -------------------
// 云端语音合成走 DashScope Qwen-TTS（与 LLM 复用同一把 QWEN_API_KEY）。
// 官方 HTTP 端点（北京地域）：POST /services/aigc/multimodal-generation/generation
//   返回 JSON，其中 output.audio.url 是 24h 有效的 .wav 下载链接（PCM 16-bit Mono）。
// 端侧流程：POST 拿 url → GET 流式下载 wav → 跳过 44B RIFF 头 → 塞 I2S。
#define QWEN_TTS_ENDPOINT      "https://dashscope.aliyuncs.com/api/v1/services/aigc/multimodal-generation/generation"
#define QWEN_TTS_MODEL         "qwen3-tts-flash"  // Flash 版，合成速度比 qwen-tts 快约 30%；REST 接口不变
#define QWEN_TTS_VOICE         "Cherry"          // 可选：Cherry/Ethan/Nofish/Jennifer/Ryan/... 详见官方文档
#define QWEN_TTS_LANGUAGE      "Chinese"         // Chinese / English / Auto
// Qwen-TTS 输出固定 24 kHz / 16-bit / Mono（官方流式示例实锤 rate=24000）。
// 此值用于 I2S 临时切采样率，播放完后恢复 TTS_I2S_DEFAULT_SAMPLE_RATE(16 kHz)。
#define QWEN_TTS_SAMPLE_RATE   24000u

// ------------------- 音量（软件增益）-------------------
// MAX98357A 硬件增益由 GAIN 引脚决定（悬空=9dB，100KΩ接GND=15dB，直接接GND=12dB）。
// 若不改硬件接线，可通过本宏在软件层对 PCM 样本乘以系数来提升音量。
//
// 计算规则：
//   1.0 = 原始音量（不放大）
//   2.0 = +6 dB  ≈ 功率翻 4 倍（推荐起点）
//   3.0 = +9.5 dB（较大音量，轻微截幅风险）
//   4.0 = +12 dB （截幅明显，仅在音源本身较安静时使用）
//
// 实现上每个 int16 样本做饱和乘法（clamp 到 ±32767），防止乘法溢出
// 导致波形翻转（那会出现"嘶嘶"失真音）。
// 注意：硬件改法更干净（无计算开销、无截幅）——若效果不满意可同时
// 把 MAX98357A 的 GAIN 引脚接一颗 100KΩ 电阻到 GND，增益跳到 15dB。
#define TTS_VOLUME_GAIN  2.0f   // 调大此值可提升音量（建议范围 1.0~4.0）

// ------------------- I2S 音频引脚配置 -------------------
// 接 MAX98357A I2S 功放模块
// Keyestudio扩展板右侧红色区域三列: S(最内侧信号)/V(中间电源)/G(最外侧地线)
// I2S接S列前3个: D4/D5/D6
#define I2S_BCLK    7   // D4/GPIO7 - I2S 位时钟 -> S列第1个
#define I2S_LRC     8   // D5/GPIO8 - I2S 帧时钟  -> S列第2个
#define I2S_DIN     9   // D6/GPIO9 - I2S 数据输入 -> S列第3个

// ------------------- 弯曲传感器配置 -------------------
// 通道数（全局常量；sensor_manager 与 calibration 均通过 config.h 获取，
// 避免模块间互相 include 产生依赖传染）
#define FLEX_CHANNEL_COUNT      5

// 开关：0 = 默认，不做任何 ADC 读取（硬件未到货 / 未接线时使用）
//       1 = 启用 5 路弯曲传感器 ADC 采集
// 若设为 1，必须同时在下方为每一路手指提供：
//   - 真实 GPIO 引脚号（FLEX_PIN_THUMB/INDEX/MIDDLE/RING/PINKY）
//   - 实测校准的 ADC 上下界（FLEX_ADC_MIN、FLEX_ADC_MAX）
// 否则编译会失败（见下方 #if 检查），以强制避免使用未经校准的假设值。
#define ENABLE_FLEX_SENSORS     0

#if ENABLE_FLEX_SENSORS
  // 以下引脚号和校准值必须在实际接线 + 实测后填写。
  // Arduino Nano ESP32 (ABX00083) 官方引脚映射供参考：
  //   A0=GPIO1, A1=GPIO2, A2=GPIO3, A3=GPIO4,
  //   A4=GPIO11 (已用于 I2C SDA), A5=GPIO12 (已用于 I2C SCL),
  //   A6=GPIO13, A7=GPIO14
  // 请注意避开 A4/A5（已被 MPU6050 的 I2C 占用）。
  //
  // 默认未提供，用户必须在启用开关前自行 #define，以下 #error 兜底：
  #if !defined(FLEX_PIN_THUMB)  || !defined(FLEX_PIN_INDEX)  || \
      !defined(FLEX_PIN_MIDDLE) || !defined(FLEX_PIN_RING)   || \
      !defined(FLEX_PIN_PINKY)
    #error "ENABLE_FLEX_SENSORS=1 requires FLEX_PIN_THUMB/INDEX/MIDDLE/RING/PINKY defined with real GPIO numbers. Please measure your wiring and define them above this #if block."
  #endif
  #if !defined(FLEX_ADC_MIN) || !defined(FLEX_ADC_MAX)
    #error "ENABLE_FLEX_SENSORS=1 requires FLEX_ADC_MIN and FLEX_ADC_MAX calibrated from real flex sensor readings (ADC value at finger straight vs fully bent)."
  #endif
  #ifndef FLEX_ADC_OVERSAMPLE
    // 软件平均采样次数；与物理校准无关，提供合理默认值
    #define FLEX_ADC_OVERSAMPLE 4
  #endif
#endif

// ------------------- 串口配置 -------------------
#define SERIAL_BAUD 115200

// ------------------- MVP 运行参数 -------------------
#define GESTURE_STABLE_MS       500     // 手势稳定时间（毫秒），达到此时间才确认识别
#define TTS_COOLDOWN_MS         2000    // 两次语音播报之间的最小间隔（毫秒）
#define SENSOR_READ_INTERVAL    50      // 传感器读取周期（毫秒），约20Hz

// ------------------- 手势仲裁层 (GestureArbitrator) -------------------
// 仲裁器坐在单手/双手识别器之上，统一决策播报内容。
// 识别器已自带 500ms 防抖，仲裁器额外加一层确认窗口 + 冷却。
//
// ARBITRATOR_CONFIRM_MS：候选需持续此时间才确认播报。
//   识别器已防抖 500ms，此处仅做帧级抖动保护（200ms ≈ 4 帧）。
#define ARBITRATOR_CONFIRM_MS   200

// ARBITRATOR_COOLDOWN_MS：播报后冷却期，期间不输出新结果。
//   与 TTS_COOLDOWN_MS 保持一致，避免重复播报。
#define ARBITRATOR_COOLDOWN_MS  2000

// ------------------- 动作/静止门控 (MotionDetector) -------------------
// 目的：手语是"动作流"，静止时刻不应触发识别；在识别器前加一级二值门控，
//       同时为未来 VAD 风格的动作分割留接口。
// 开关：0 = 关闭门控，保持旧行为（每帧都喂给识别器）
//       1 = 启用门控，STILL 状态下主循环跳过识别（TTS/CSV 正常）
#define ENABLE_MOTION_GATING    1

// 滑动窗口大小（帧）。20Hz 采样下 10 帧 = 0.5s 窗口，足以覆盖一次挥手的
// 起/止边沿。为避免动态分配，MotionDetector 内部用定长数组，因此此值必须
// 是编译期常量，且与下面 MOTION_WIN_MAX 保持一致（或更小）。
#define MOTION_WIN_SIZE         10
#define MOTION_WIN_MAX          32      // MotionDetector 内部数组上限

// 双阈值滞回（进入 MOVING 用 high 阈值，退出用 low 阈值），避免临界抖动。
// 依据：
//   - MPU6050 datasheet 标称加速度输出噪声 ~400 µg/√Hz，在 20Hz 带宽下
//     单轴 RMS ≈ 0.004 g，|a| 方差量级 1e-5 g²（静止）。运动时单轴瞬时
//     变化轻松达 0.3 g，|a| 方差量级 1e-2 g²。
//   - 陀螺仪 datasheet 噪声 ~0.05°/s/√Hz，20Hz 带宽下 RMS ≈ 0.22°/s。
// 默认值取上述噪声的 ~5~10× 做裕量；最终值 **需在真实佩戴条件下用采集
// 模式录制静止段 / 运动段后二次调参**，config.h 注释此处留给调参者。
#define MOTION_VAR_ENTER        0.005f  // |a| 方差 > 该值 → MOVING   (g²)
#define MOTION_VAR_EXIT         0.002f  // |a| 方差 < 该值 → 允许回 STILL
#define MOTION_GYRO_ENTER       15.0f   // |gyro| > 该值 → MOVING     (deg/s)
#define MOTION_GYRO_EXIT        5.0f    // |gyro| < 该值 → 允许回 STILL

// 状态切回 STILL 需要连续满足 EXIT 条件的最少帧数（防抖）
#define MOTION_STILL_HOLD_FRAMES 6      // 6 * 50ms = 300ms 静止才算真静止

// ------------------- 功能开关 -------------------
// MVP阶段：设为 0 跳过LLM测试，直接走 传感器→识别→TTS 链路
// 设为 1 则在 setup 中额外测试 LLM 连通性
#define ENABLE_LLM_TEST         0

// ------------------- LLM 改写（手势 → 自然句） -------------------
// 在识别到手势/手势序列之后，先调 Qwen LLM 把"词/字母/短序列"改写成
// 一句自然中文口语，再交给 TTS 播报。失败/超时/未联网时回落到原始词，
// 保证链路永远可用。
//   0 = 关闭（识别到什么就播什么，保留 MVP 最小链路）
//   1 = 开启（默认；需要 WiFi + QWEN_API_KEY）
#define ENABLE_LLM_REWRITE      1

// 最终改写结果的长度上限（字节）。超过则视为异常（LLM 胡乱发挥 / 把 prompt
// 回声了），丢弃本次改写、回落到原始词。
// 25 字汉语 ≈ 75 UTF-8 字节，留一倍余量到 160。
#define LLM_REWRITE_MAX_BYTES   160

// 改写 prompt 模板。%s 会被替换为手势序列字符串（如 "你好" 或 "我,吃饭"）。
// 原则：
//   - 明确只输出一句话本身（避免模型返回 "好的，这句话是："前缀）
//   - 限制长度在 25 汉字内，符合 TTS 合成时延预算（24 kHz WAV ~1s/5字）
//   - 要求口语化，符合"把手语词序补全为自然句"的真实使用场景
#define LLM_REWRITE_PROMPT_TEMPLATE \
    "你是手语翻译助手。下面按时间顺序给出识别到的中文手语词或字母序列：" \
    "「%s」。" \
    "请把它改写成一句最自然、最简短的中文口语（不超过 25 字）。" \
    "只输出这句话本身，不要任何解释、前缀、引号、括号或换行。"

// ------------------- ESP-NOW 双手同步 -------------------
// 目的：为"双手手语翻译"方案提供底层通信层。启用后，从手（SLAVE）通过
//       ESP-NOW 广播 HandFrame 给主手（MASTER），主手做特征拼接与识别。
// 0 = 关闭（MVP 单手行为不受影响，esp_now_sync.* 编译为空 stub）
// 1 = 启用（会引入 <esp_now.h> <WiFi.h> 依赖）
// 注意：与在线 TTS / LLM 共用同一 WiFi 模块，两块板须在同一 WiFi 信道。
//       MASTER 连 WiFi AP 后信道由 AP 决定，SLAVE 将跟随同一信道自动配对。
#define ENABLE_ESPNOW_SYNC      1

// ESPNOW_ROLE：烧录时区分 MASTER / SLAVE 固件的唯一宏
//   0 = MASTER（右手）：连 WiFi + TTS 播报 + 收 Slave 帧 + 双手识别
//   1 = SLAVE （左手）：仅 ESP-NOW 广播 HandFrame，不走 WiFi / TTS
//
// 推荐用法：在 sketch 目录下创建 build_opt.h，写入 -DESPNOW_ROLE=0 或 =1，
//   Arduino 构建系统会自动注入编译选项，无需修改源代码即可切换角色。
//   模板文件见 build_opt.h.master / build_opt.h.slave。
#ifndef ESPNOW_ROLE
#define ESPNOW_ROLE             0   // 默认 MASTER；可通过 build_opt.h 覆盖
#endif

// ------------------- 双手协同识别阈值 -------------------
// 以下阈值仅在 ENABLE_ESPNOW_SYNC=1 && ESPNOW_ROLE=0 (MASTER) 时生效。
//
// BIMANUAL_SLAVE_STALE_MS：Slave 帧超过此时间未刷新即视为"失联/超时"，
//   双手识别器不输出结果，避免用过期数据做判定。
//   50ms 帧周期下 200ms ≈ 4 帧容忍丢包，实测可按信号质量调整。
#define BIMANUAL_SLAVE_STALE_MS     200

// BIMANUAL_PITCH_THRESHOLD_DEG：双手 pitch 需同时超过此阈值才进入"加油"候选。
//   30° 远小于单手识别的 45°，给非标准姿势留裕量；
//   具体动作"双手上抬握拳"时 pitch 典型值 40°–70°，阈值 30° 有充足余量。
#define BIMANUAL_PITCH_THRESHOLD_DEG  30.0f

// BIMANUAL_PITCH_DOWN_THRESHOLD_DEG：双手 pitch 同时低于此负值 → "一起"候选。
//   动作为双手掌心朝下向前推出，pitch 典型值 -40°~-70°。
#define BIMANUAL_PITCH_DOWN_THRESHOLD_DEG  (-30.0f)

// BIMANUAL_ROLL_THRESHOLD_DEG：双手 roll 对称偏转的绝对值阈值 → "我爱你"候选。
//   动作为双手交叉置胸口，右手左倾(roll>0)、左手右倾(roll<0)。
#define BIMANUAL_ROLL_THRESHOLD_DEG  30.0f

// BIMANUAL_STABLE_MS：双手同时满足条件需持续此时间才触发，防抖用。
//   与单手 GESTURE_STABLE_MS(500ms) 保持一致，可独立调整。
#define BIMANUAL_STABLE_MS          500

// BIMANUAL_HELP_SLAVE_PITCH_DEG：「帮助」中左手掌朝上托举，pitch 需大于此阈值。
//   动作"左手掌朝上托举"时 pitch 典型值 +30°~+60°。
#define BIMANUAL_HELP_SLAVE_PITCH_DEG     30.0f

// BIMANUAL_HELP_MASTER_NEUTRAL_DEG：「帮助」中右手握拳置左掌心，pitch/roll 都需在
//   ±此阈值的中性区内（避免与「加油」「一起」「我爱你」冲突）。
#define BIMANUAL_HELP_MASTER_NEUTRAL_DEG  20.0f

// ------------------- 准确率测试模式参数 -------------------
// 用于 'test <id> <count>' 串口命令进入的离线评测模式（不走 LLM/TTS）。
// 单次手势的最大等待时长：到点未检测到视为漏报（miss）。给用户足够时间
// 摆姿势（含 BIMANUAL_STABLE_MS=500ms 的防抖），4000ms 实测够用。
#define ACCURACY_TEST_SLOT_TIMEOUT_MS   4000

// 单次手势结束后，要求用户回到 STILL 持续此时间才进入下一轮。
// 防止"误触发→连环误触发"——必须明确放松手再做下一次。
#define ACCURACY_TEST_REST_HOLD_MS      400

// 单次测试会话最多支持的手势次数（用于栈上数组上限）。
#define ACCURACY_TEST_MAX_ATTEMPTS      50

// MPU6050 加速度/陀螺仪原始值 → pitch 角度换算比例（FullScale ±2g, 16384 LSB/g）
// 用于 HandFrame 里的 int16 原始值重建 pitch（MASTER 侧换算 Slave 的原始帧）：
//   pitch_deg = atan2(-ax_raw / 16384.0, az_raw / 16384.0) * (180 / PI)
// 此常量由 HandFrame 的 IMU 量程决定，不随实测而变化。
#define MPU6050_ACCEL_SCALE_G       16384.0f  // LSB per g (±2g full scale)

// ------------------- ESP-NOW LED 指示灯 -------------------
// 启用后，SLAVE 每成功发送一帧 toggle LED，MASTER 每收到一帧 toggle LED。
// 20Hz 帧率下 LED 约 10Hz 闪烁，肉眼可见"常亮"效果；通信中断则 LED 停止变化。
// 使用板载 LED_BUILTIN（Arduino Nano ESP32-S3 = GPIO48 / D13），无需外接硬件。
#define ESPNOW_LED_INDICATOR    1
// Arduino Nano ESP32-S3 RST 按键旁 RGB LED（三独立 GPIO，低电平有效）：
//   Red=GPIO46(D14/LED_RED), Green=GPIO0(D15/LED_GREEN), Blue=GPIO45(D16/LED_BLUE)
// 使用 digitalWrite(LED_BLUE, LOW) 点亮蓝色，HIGH 熄灭。
#define ESPNOW_LED_PIN          LED_BLUE

// ------------------- TTS 本地缓存 -------------------
// 启用后，speak() 会优先从 LittleFS 读取已缓存的 WAV 文件；
// 缓存未命中时走云端合成，合成成功后自动写入缓存供下次使用。
// 缓存目录：/tts_cache/，文件名为文本 FNV-1a 哈希的 hex 字符串 + ".wav"。
// 0 = 关闭（每次都走云端，不读写 Flash）
// 1 = 启用（推荐，10 句演示词汇缓存后 TTS 延迟从 2-4s → <100ms）
#define TTS_CACHE_ENABLE        1

// ------------------- 调试开关 -------------------
// 设为 1 开启详细日志，设为 0 关闭
#define DEBUG_MODE  1

// 日志宏说明：
//   DEBUG_LOG(fmt, ...)  — 主日志接口。在行首自动附加 "[<millis>ms] " 时间戳，
//                          使用 printf 格式化字符串，支持多参数拼接，输出带换行。
//                          示例：DEBUG_LOG("[TTS] 请求合成: %s", text);
//                                 DEBUG_LOG("[HTTP] 状态码: %d", code);
//   DEBUG_PRINTLN(x)    — 兼容旧调用。等价于 DEBUG_LOG("%s", String(x).c_str())，
//                          仅支持单参数（不支持多段拼接），用于纯字符串日志。
//   DEBUG_PRINT(x)      — 无时间戳，无换行，仅用于 CSV 数据流等特殊场合（尽量避免）。
#if DEBUG_MODE
  // ets_printf 对含 UTF-8 中文字节的格式串存在截断风险（高位字节被误读为格式符），
  // 且与 HardwareSerial 缓冲区是两路 UART 通道，混用会导致输出交织。
  // 修复：用 snprintf 先把整行（时间戳+正文+\n）拼到栈缓冲区，
  //       再一次性 Serial.write(buf, n) 输出——snprintf 对 UTF-8 字节透明，
  //       Serial.write 是单次缓冲写，完全原子，不走 ets_printf。
  #define DEBUG_LOG(fmt, ...)  do {                                              \
      char _dbg_buf[256];                                                        \
      int _dbg_n = snprintf(_dbg_buf, sizeof(_dbg_buf) - 1,                    \
                            "[%lums] " fmt "\r\n",                              \
                            (unsigned long)millis(), ##__VA_ARGS__);            \
      if (_dbg_n > 0) {                                                         \
          if (_dbg_n >= (int)sizeof(_dbg_buf)) _dbg_n = sizeof(_dbg_buf) - 1;  \
          Serial.write((const uint8_t*)_dbg_buf, (size_t)_dbg_n);              \
      }                                                                         \
    } while (0)
  #define DEBUG_PRINT(...)     Serial.print(__VA_ARGS__)
  // DEBUG_PRINTLN 统一走 DEBUG_LOG，消除 Serial.print+Serial.println 混用
  #define DEBUG_PRINTLN(msg)   DEBUG_LOG("%s", (msg))
#else
  #define DEBUG_LOG(fmt, ...)  ((void)0)
  #define DEBUG_PRINT(...)     ((void)0)
  #define DEBUG_PRINTLN(...)   ((void)0)
#endif

#endif // CONFIG_H
