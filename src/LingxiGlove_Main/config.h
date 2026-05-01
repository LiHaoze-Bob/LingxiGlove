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
#define QWEN_TTS_MODEL         "qwen-tts"        // 稳定版；亦可改 "qwen3-tts-flash"
#define QWEN_TTS_VOICE         "Cherry"          // 可选：Cherry/Ethan/Nofish/Jennifer/Ryan/... 详见官方文档
#define QWEN_TTS_LANGUAGE      "Chinese"         // Chinese / English / Auto
// Qwen-TTS 输出固定 24 kHz / 16-bit / Mono（官方流式示例实锤 rate=24000）。
// 此值用于 I2S 临时切采样率，播放完后恢复 TTS_I2S_DEFAULT_SAMPLE_RATE(16 kHz)。
#define QWEN_TTS_SAMPLE_RATE   24000u

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

// ------------------- ESP-NOW 双手同步（A 阶段接口预埋） -------------------
// 目的：为"双手手语翻译"方案预留底层通信层。启用后，从手（SLAVE）通过
//       ESP-NOW 广播 HandFrame 给主手（MASTER），主手做特征拼接与识别。
// 0 = 关闭（默认，MVP 单手行为不受影响，esp_now_sync.* 编译为空 stub）
// 1 = 启用（会引入 <esp_now.h> <WiFi.h> 依赖，并占用 WiFi 模块）
// 注意：与现有在线 TTS / LLM 功能共用同一个 WiFi 模块，启用前需评估 Wi-Fi
//       与 ESP-NOW 混用对带宽/延迟的影响（需同信道）。
#define ENABLE_ESPNOW_SYNC      0

// ------------------- 调试开关 -------------------
// 设为 1 开启详细日志，设为 0 关闭
#define DEBUG_MODE  1

// 使用可变参数宏，兼容 DEBUG_PRINTLN(x) 和 DEBUG_PRINTLN(x, HEX) 两种调用形式
#if DEBUG_MODE
  #define DEBUG_PRINT(...)    Serial.print(__VA_ARGS__)
  #define DEBUG_PRINTLN(...)  Serial.println(__VA_ARGS__)
#else
  #define DEBUG_PRINT(...)    ((void)0)
  #define DEBUG_PRINTLN(...)  ((void)0)
#endif

#endif // CONFIG_H
