// ============================================================
// LingxiGlove 主程序
// 灵犀手套 - 智能手语翻译系统
// ============================================================
// MVP阶段：基于 MPU6050 验证完整链路
//   传感器读取 → 手势识别（规则-based） → 文本 → 云端TTS → I2S播放
// ============================================================
// 硬件: Arduino Nano ESP32-S3 + MPU6050 + I2S音频模块
// ============================================================

#include "config.h"
#include "wifi_manager.h"
#include "sensor_manager.h"
#include "gesture_recognizer.h"
#include "tts_player.h"
#include "motion_detector.h"
#include "calibration.h"
#include "local_tts_fallback.h"
#include "esp_now_sync.h"

// LLM 客户端：除 ENABLE_LLM_TEST 启动自检外，主循环里的 LLM 改写层
// （rewriteGestureToSentence）与串口命令 'l' 都需要它，统一无条件 include。
#include "llm_client.h"

// math.h 用于 BimanualInput 的 pitch 换算（atan2f）
#include <math.h>

// ------------------- 运行模式 -------------------
enum RunMode {
    MODE_RECOGNIZE       = 0,   // 正常识别 + TTS 播报
    MODE_CAPTURE         = 1,   // 数据采集：CSV 输出用于 Edge Impulse 训练（词级手势）
    MODE_FINGER_SPELLING = 2    // 指拼数据采集：CSV 输出，为后续指拼字母表模型预留
                                // 当前阶段识别器不处理此模式，避免用姿态角规则伪造字母识别结果；
                                // 待指拼模型训练完成后，在此模式下接入专用识别器
    // 说明：校准是一次性阻塞流程（~3s），设计上放在 handleSerialCommand 内同步执行，
    // 不引入独立的 MODE_CALIBRATING 状态（loop() 此时天然被阻塞在 runCalibrationFlow 里）
};

// ------------------- 前置声明 -------------------
static void printBanner();
static void haltWithError();
static void printHelp();
static void handleSerialCommand();
static void printCsvHeader();
static void printCsvRow(const SensorData& data);
static void doRecognizeStep(const SensorData& data, unsigned long now);
static void runCalibrationFlow();
static bool readSampleAdapter(float* ax, float* ay, float* az,
                              float* gx, float* gy, float* gz);
#if ENABLE_FLEX_SENSORS
static bool readFlexRawAdapter(uint16_t out_flex[FLEX_CHANNEL_COUNT]);
#endif

// 全局对象
static GestureRecognizer* g_recognizer = nullptr;
static MotionDetector     g_motionDetector;
static CalibrationData    g_cal;  // 启动时从 NVS 加载，由 'k' 命令更新

// 运行状态
static GestureType    g_lastAnnouncedGesture = GESTURE_NONE;
static unsigned long  g_lastAnnounceTime = 0;
static unsigned long  g_lastSensorRead = 0;
static int            g_debugCounter = 0;
static bool           g_systemReady = false;
static RunMode        g_runMode = MODE_RECOGNIZE;
#if ENABLE_MOTION_GATING
static MotionState    g_lastMotionState = MOTION_STATE_STILL;
#endif

// ============================================================
// ESP-NOW 双手协同状态（ENABLE_ESPNOW_SYNC=1 时生效）
// ============================================================
#if ENABLE_ESPNOW_SYNC

#if ESPNOW_ROLE == 0  // MASTER

// 最新收到的 Slave 帧及其接收时刻（由 ESP-NOW 接收回调写入，主循环读取）
// 注意：ESP-NOW 回调在系统任务上下文中执行，与 loop() 并发。
// 此处用 volatile 修饰接收时刻，防止编译器优化掉对 g_slave_frame_rx_ms 的读取。
// HandFrame 本身的写入是单次 memcpy（32 字节对齐），ESP32 单核写不会被主循环
// 中途看到半段数据；双核场景下最坏情况是读到"旧帧+新时刻"组合，对本阶段
// 验证 Demo 可接受（不影响安全，只影响一帧判断延迟）。
static HandFrame         g_slave_frame;
static volatile uint32_t g_slave_frame_rx_ms = 0;  // 0 表示还未收到任何帧
static bool              g_slave_frame_valid  = false;

// 双手规则识别器（在 MASTER 上实例化）
static BimanualRuleRecognizer g_bimanual_recognizer;

// 双手识别冷却计时（与单手 TTS_COOLDOWN_MS 共用 g_lastAnnounceTime，
// 避免单手和双手识别结果互相干扰播报节奏）
// 无需独立变量，由 doRecognizeStep 内的 g_lastAnnounceTime 统一管控。

/**
 * @brief 将 HandFrame 的 int16 原始 ax/az 换算为 pitch 角（度）
 *
 * MPU6050 FullScale ±2g 时 1 LSB = 1/16384 g。
 * pitch = atan2(-ax, az) × (180/π)
 * 负号约定：与 sensor_manager.cpp 中 computeOrientation 的符号保持一致，
 * 手背朝上/掌心向下时 pitch 为负；手掌朝上时 pitch 为正。
 */
static float HandFrameToPitch(const HandFrame& frame) {
    float ax_g = (float)frame.ax / MPU6050_ACCEL_SCALE_G;
    float az_g = (float)frame.az / MPU6050_ACCEL_SCALE_G;
    return atan2f(-ax_g, az_g) * (180.0f / (float)M_PI);
}

/**
 * @brief ESP-NOW 接收回调（系统任务上下文）
 *
 * 严禁在此函数内做任何阻塞操作（Serial.print 除外的 I/O、delay 等）。
 * 仅做帧缓冲写入和时刻记录。
 */
static void OnSlaveHandFrame(const HandFrame& frame, const uint8_t /*mac*/[6]) {
    memcpy(&g_slave_frame, &frame, sizeof(HandFrame));
    g_slave_frame_rx_ms = (uint32_t)millis();
    g_slave_frame_valid = true;
}

#endif  // ESPNOW_ROLE == 0 (MASTER)
#endif  // ENABLE_ESPNOW_SYNC

// ============================================================
// setup() - 系统初始化
// ============================================================
void setup() {
    Serial.begin(SERIAL_BAUD);
    while (!Serial) { ; }
    delay(500);

    printBanner();

    // ---------- 1. 初始化传感器 ----------
    DEBUG_PRINTLN("[系统] 正在初始化传感器...");
    if (!initSensors()) {
        DEBUG_PRINTLN("[系统] 错误: 传感器初始化失败，请检查 MPU6050 接线");
        DEBUG_PRINTLN("  预期接线: SDA->A4/GPIO11, SCL->A5/GPIO12");
        haltWithError();
    }
    DEBUG_PRINTLN("[系统] 传感器初始化成功");

    // ---------- 1.5 加载并应用个体校准 ----------
    // 未校准/首次启动时 LoadCalibration 返回 false，g_cal 被 Reset 为全零，
    // ApplyCalibration 则把 sensor_manager 的偏移/量程也写回默认值。
    if (LoadCalibration(&g_cal)) {
        DEBUG_PRINTLN("[系统] 个体校准已从 NVS 加载");
        PrintCalibration(g_cal);
    } else {
        DEBUG_PRINTLN("[系统] 未找到有效校准（首次启动或版本失配），使用默认值");
        DEBUG_PRINTLN("        建议按 'k' 进行一次零偏校准以提升识别稳定性");
    }
    ApplyCalibration(g_cal);

    // ---------- 2. 初始化手势识别器 ----------
    DEBUG_PRINTLN("[系统] 正在初始化手势识别器...");
    g_recognizer = createGestureRecognizer();
    if (!g_recognizer) {
        DEBUG_PRINTLN("[系统] 错误: 无法创建手势识别器");
        haltWithError();
    }
    if (!g_recognizer->init()) {
        DEBUG_PRINTLN("[系统] 错误: 手势识别器初始化失败");
        haltWithError();
    }
    DEBUG_LOG("[系统] 手势识别器就绪: %s", g_recognizer->getName());

// ============================================================
// SLAVE 固件从此处分叉：仅初始化 MPU6050 + ESP-NOW，然后进入独立主循环
// MASTER 固件继续执行后续 I2S / WiFi / TTS 初始化
// ============================================================
#if ENABLE_ESPNOW_SYNC && (ESPNOW_ROLE == 1)  // ---------- SLAVE 启动路径 ----------

    // SLAVE 不需要 I2S / WiFi / TTS / LLM，setup() 在此处完成并返回。
    // loop() 中会走 SLAVE 的采样→发帧路径，与 MASTER 路径互斥（同一宏控制）。
    DEBUG_PRINTLN("[Slave] 正在初始化 ESP-NOW...");
    // SLAVE 用广播 MAC 发帧，无需提前知道 MASTER 地址
    if (!InitEspNowSync(ESPNOW_ROLE_SLAVE, nullptr)) {
        DEBUG_PRINTLN("[Slave] 错误: ESP-NOW 初始化失败，系统停止");
        haltWithError();
    }
    DEBUG_PRINTLN("[Slave] ESP-NOW 初始化成功，开始广播 HandFrame...");
    g_systemReady = true;
    return;  // SLAVE setup() 到此结束

#else  // ---------- MASTER 启动路径（含非 ESPNOW 的单手 MVP）----------

    // ---------- 3. 初始化 I2S 音频 ----------
    DEBUG_PRINTLN("[系统] 正在初始化 I2S 音频...");
    if (!initTTS()) {
        DEBUG_PRINTLN("[系统] 警告: I2S 音频初始化失败，语音播报不可用");
        DEBUG_PRINTLN("  预期接线: BCLK->D4/GPIO7, LRC->D5/GPIO8, DIN->D6/GPIO9");
        // 非致命错误，继续运行
    } else {
        DEBUG_PRINTLN("[系统] I2S 初始化成功，播放开机提示音...");
        playTestTone(880, 200);   // A5
        delay(100);
        playTestTone(1100, 200);  // C#6
    }

    // ---------- 4. 连接 WiFi ----------
    DEBUG_PRINTLN("[系统] 正在连接 WiFi...");
    bool wifiOk = false;
    while (!wifiOk) {
        wifiOk = connectWiFi(WIFI_SSID, WIFI_PASSWORD, WIFI_TIMEOUT_MS);
        if (!wifiOk) {
            DEBUG_PRINTLN("[系统] WiFi 连接失败，5秒后重试...");
            delay(5000);
        }
    }
    // WiFi 连上后信道已确定，MASTER 现在才初始化 ESP-NOW，
    // 确保 ESP-NOW 与 WiFi 使用同一信道（ESP-NOW channel=0 表示跟随当前 WiFi 信道）
#if ENABLE_ESPNOW_SYNC  // MASTER 路径下 ESPNOW_ROLE == 0
    DEBUG_PRINTLN("[Master] 正在初始化 ESP-NOW...");
    if (!InitEspNowSync(ESPNOW_ROLE_MASTER, nullptr)) {
        // MASTER 要求 peer_mac 非 nullptr，但此处先以广播 peer 启动，
        // Slave 广播帧到达后回调会自然触发。
        // InitEspNowSync 对 MASTER+nullptr 返回 false，改用强制广播模式：
        // 注册广播 peer，让 MASTER 也能收到 Slave 的广播帧。
        // ESP-NOW 接收不依赖 peer 注册，只需注册 recv_cb 即可；
        // 此处绕过 InitEspNowSync 的 MASTER 校验，直接底层注册接收回调。
        // ——但为了复用已有接口，实际做法：MASTER 以 SLAVE 角色初始化 ESP-NOW
        //   只为注册接收回调，然后 RegisterHandFrameHandler 注入自己的处理器。
        // 更简洁：修改策略——MASTER 不发帧，只收帧，peer 注册可以延迟到
        //   收到第一帧 Slave MAC 后再添加。此处仅调 esp_now_init + register_recv_cb。
        // 由于 InitEspNowSync 封装太严，直接裁剪：将 ESPNOW_ROLE_SLAVE 传入，
        // 让其注册广播 peer（SLAVE 模式允许 nullptr），这样 MASTER 就能收到广播帧。
        if (!InitEspNowSync(ESPNOW_ROLE_SLAVE, nullptr)) {
            DEBUG_PRINTLN("[Master] 警告: ESP-NOW 初始化失败，双手识别不可用");
        } else {
            RegisterHandFrameHandler(OnSlaveHandFrame);
            g_bimanual_recognizer.init();
            DEBUG_PRINTLN("[Master] ESP-NOW 初始化成功（接收模式）");
        }
    } else {
        RegisterHandFrameHandler(OnSlaveHandFrame);
        g_bimanual_recognizer.init();
        DEBUG_PRINTLN("[Master] ESP-NOW 初始化成功，等待 Slave 帧...");
    }
#endif  // ENABLE_ESPNOW_SYNC

#if ENABLE_LLM_TEST
    // ---------- 5. 可选：测试 LLM 连通性 ----------
    DEBUG_PRINTLN("[系统] 测试 LLM 连通性...");
    if (initLLM()) {
        String reply = chatLLM("你好，请用一句话打招呼");
        DEBUG_LOG("[LLM] 测试回复: %s", reply.c_str());
    } else {
        DEBUG_PRINTLN("[系统] 警告: LLM 初始化失败");
    }
#endif

    // ---------- 系统就绪 ----------
    g_systemReady = true;
    DEBUG_PRINTLN("\n============================================");
    DEBUG_PRINTLN("  链路就绪，开始手势识别...");
    DEBUG_PRINTLN("============================================");
#if ENABLE_ESPNOW_SYNC
    DEBUG_PRINTLN("  [双手模式] Master: 单手手势 + 双手协同（加油）");
    DEBUG_PRINTLN("  双手协同：双手同时抬起（pitch > 30°）持续 0.5s → 加油");
#else
    DEBUG_PRINTLN("  支持手势: 朝上=你好 | 朝下=谢谢 | 左倾=再见 | 右倾=是 | 竖直=不");
#endif
    printHelp();
    DEBUG_PRINTLN("============================================\n");

    // 播放就绪提示语音
    // WiFi 刚连上时 TLS 栈可能还没热身，第一次失败后等 2s 重试一次
    if (isWiFiConnected()) {
        if (!speak("灵犀手套已就绪")) {
            DEBUG_PRINTLN("[系统] 开机 TTS 失败，2s 后重试一次...");
            delay(2000);
            speak("灵犀手套已就绪");
        }
    }

#endif  // MASTER 启动路径结束
}

// ============================================================
// loop() - 主循环
//   MODE_RECOGNIZE       : 采集 → 识别 → TTS 播报
//   MODE_CAPTURE         : 采集 → CSV 输出（词级手势训练数据）
//   MODE_FINGER_SPELLING : 采集 → CSV 输出（指拼字母表训练数据，识别不启用）
// ============================================================
void loop() {
    if (!g_systemReady) {
        delay(1000);
        return;
    }

    // 串口命令处理（非阻塞）
    handleSerialCommand();

    unsigned long now = millis();

    // 按固定周期读取传感器（约 20Hz）
    if (now - g_lastSensorRead < (unsigned long)SENSOR_READ_INTERVAL) {
        delay(2);
        return;
    }
    g_lastSensorRead = now;

    // ---------- 1. 读取传感器 ----------
    SensorData data;
    if (!readSensors(data)) {
        DEBUG_PRINTLN("[系统] 传感器读取失败");
#if !(ENABLE_ESPNOW_SYNC && (ESPNOW_ROLE == 1))
        checkWiFiConnection(WIFI_SSID, WIFI_PASSWORD);
#endif
        return;
    }

// ============================================================
// SLAVE 主循环：采样 → 打包 HandFrame → 广播发出，不走 WiFi/TTS
// ============================================================
#if ENABLE_ESPNOW_SYNC && (ESPNOW_ROLE == 1)

    {
        static uint16_t s_slave_seq = 0;
        HandFrame frame;
        memset(&frame, 0, sizeof(frame));
        // master_timestamp_ms 由 MASTER 打戳；SLAVE 端始终填 0
        frame.master_timestamp_ms = 0;
        frame.seq_no              = s_slave_seq++;
        // SensorData.accelX 是物理值（g），反推 int16 LSB = value × 16384
        frame.ax = (int16_t)(data.accelX * MPU6050_ACCEL_SCALE_G);
        frame.ay = (int16_t)(data.accelY * MPU6050_ACCEL_SCALE_G);
        frame.az = (int16_t)(data.accelZ * MPU6050_ACCEL_SCALE_G);
        // gyroX 是物理值（deg/s），陀螺 ±250°/s full-scale → 1LSB = 1/131 °/s
        frame.gx = (int16_t)(data.gyroX * 131.0f);
        frame.gy = (int16_t)(data.gyroY * 131.0f);
        frame.gz = (int16_t)(data.gyroZ * 131.0f);
#if ENABLE_FLEX_SENSORS
        for (uint8_t ch = 0; ch < FLEX_CHANNEL_COUNT; ch++) {
            frame.flex[ch] = data.flex[ch];
        }
#endif
        if (!SendHandFrame(frame)) {
            DEBUG_PRINTLN("[Slave] 发帧失败（ESP-NOW 入队错误）");
        } else {
            // 每 50 帧打印一次统计，避免刷屏
            static uint16_t s_print_counter = 0;
            if (++s_print_counter >= 50) {
                s_print_counter = 0;
                DEBUG_LOG("[Slave] 已发 %u 帧  pitch=%.1f  seq=%u",
                          (unsigned)GetEspNowTxCount(),
                          (double)data.pitch,
                          (unsigned)frame.seq_no);
            }
        }
    }

#else  // MASTER 主循环（含非 ESPNOW 的单手 MVP）

    // ---------- 2. 分模式处理 ----------
    // CAPTURE 与 FINGER_SPELLING 共享 CSV 输出通路；两者的数据差异仅体现在
    // 采集时的"标签语义"（词 vs 字母）上，由上位机采集脚本/Edge Impulse 项目区分
    if (g_runMode == MODE_CAPTURE || g_runMode == MODE_FINGER_SPELLING) {
        printCsvRow(data);
    } else {
        doRecognizeStep(data, now);
    }

    // ---------- 3. 维护 WiFi 连接 ----------
    checkWiFiConnection(WIFI_SSID, WIFI_PASSWORD);

#endif  // MASTER 主循环结束
}

// ============================================================
// 识别模式单步处理
// ============================================================
static void doRecognizeStep(const SensorData& data, unsigned long now) {
#if ENABLE_MOTION_GATING
    // 动作/静止门控：静止时直接跳过识别器，避免对静止姿势反复"命中"规则
    // 且为未来 "动作分割+分类" 两段式推理链留一个清晰的切点
    MotionSample ms;
    ms.accel_x = data.accelX;
    ms.accel_y = data.accelY;
    ms.accel_z = data.accelZ;
    ms.gyro_x  = data.gyroX;
    ms.gyro_y  = data.gyroY;
    ms.gyro_z  = data.gyroZ;
    MotionDecision md = g_motionDetector.Update(ms);

    if (md.state_changed) {
        DEBUG_LOG("[门控] 状态切换: %s  var(|a|)=%.5f  |gyro|=%.2f",
                  md.state == MOTION_STATE_MOVING ? "STILL→MOVING" : "MOVING→STILL",
                  (double)md.accel_mag_variance,
                  (double)md.gyro_magnitude);
        g_lastMotionState = md.state;
    }

    if (md.state == MOTION_STATE_STILL) {
        // 静止窗口期：不调用识别器；允许再次触发同一手势
        g_lastAnnouncedGesture = GESTURE_NONE;
        return;
    }
#endif

    // 手势识别
    GestureResult result = g_recognizer->recognize(data);

    // 调试输出（每 10 次打印一次，避免刷屏）
    if (++g_debugCounter >= 10) {
        g_debugCounter = 0;
        printSensorData(data);
    }

    // 触发语音播报
    if (result.type != GESTURE_NONE &&
        result.type != g_lastAnnouncedGesture &&
        now - g_lastAnnounceTime > (unsigned long)TTS_COOLDOWN_MS) {

        DEBUG_LOG("\n[识别] 检测到手势: %s (置信度: %.2f)", result.text, (double)(result.confidence));

        // ----- LLM 改写层 -----
        // 把识别到的"词/短序列"交给 Qwen 改写为一句自然中文口语（如 "吃饭"
        // → "我想吃饭"）。失败/未联网/超长时 rewriteGestureToSentence 返回空
        // String，此处回落到原始 result.text，确保播报链路永远可用。
        // 这里用栈上 char[] 做为最终送入 speak() 的指针，避免 String 跨作用域
        // 引用；spoken_text 要么指向 result.text，要么指向 rewritten.c_str()，
        // rewritten 的生命周期覆盖整个 if 块。
        const char* spoken_text = result.text;
        String rewritten;
#if ENABLE_LLM_REWRITE
        if (WiFi.status() == WL_CONNECTED) {
            rewritten = rewriteGestureToSentence(result.text);
            if (rewritten.length() > 0) {
                DEBUG_LOG("[识别] LLM 改写为自然句: %s", rewritten.c_str());
                spoken_text = rewritten.c_str();
            } else {
                DEBUG_PRINTLN("[识别] LLM 改写失败/未启用，回落原始手势词");
            }
        } else {
            DEBUG_PRINTLN("[识别] WiFi 未就绪，跳过 LLM 改写");
        }
#endif

        // 两级播报：云端 TTS 失败 → 离线 PCM 查表兜底
        // 两级都失败才算播报失败；不在此处做蜂鸣兜底，避免把"无可用语音"伪装成"正常播报"
        // 注意：离线兜底的 label 仍用 result.text（手势词原文），因为离线 PCM
        // 表是按"手势词 → 预录音频"建立的，改写后的自然句不会命中。
        bool spoken = speak(spoken_text);
        if (!spoken) {
            DEBUG_PRINTLN("[系统] 在线 TTS 失败，尝试离线兜底 ...");
            if (PlayOfflineVoice(result.text)) {
                DEBUG_PRINTLN("[系统] 离线兜底播报成功");
                spoken = true;
            } else if (OfflineVoiceCount() == 0) {
                DEBUG_PRINTLN("[系统] 离线 PCM 表为空，未配置兜底数据 (见 tools/gen_offline_voice_pcm.py)");
            } else {
                DEBUG_LOG("[系统] 离线兜底也未命中 label='%s'", result.text);
            }
        }
        if (!spoken) {
            DEBUG_PRINTLN("[系统] 全部播报通道失败，本次识别无声音输出");
        }

        g_lastAnnouncedGesture = result.type;
        g_lastAnnounceTime = now;
    }

    // 手势复位：当回到无手势状态时，允许再次识别同一手势
    if (result.type == GESTURE_NONE) {
        g_lastAnnouncedGesture = GESTURE_NONE;
    }

// ============================================================
// Master 双手协同识别（ENABLE_ESPNOW_SYNC=1 && ESPNOW_ROLE=0）
// 在单手识别之后单独运行；两者共用 g_lastAnnounceTime 冷却计时，
// 防止单手和双手结果在同一冷却窗口内互相覆盖。
// ============================================================
#if ENABLE_ESPNOW_SYNC && (ESPNOW_ROLE == 0)
    {
        // 计算 Slave 帧的"年龄"（距收到时刻的毫秒数）
        unsigned long slave_age_ms = 0;
        if (g_slave_frame_valid) {
            uint32_t rx_ms = g_slave_frame_rx_ms;  // 读一次 volatile，避免多次读取不一致
            slave_age_ms = (now >= rx_ms) ? (now - rx_ms) : 0;
        } else {
            // 从未收到过 Slave 帧，直接设为超出阈值，让识别器立即返回 NONE
            slave_age_ms = BIMANUAL_SLAVE_STALE_MS + 1;
        }

        // 从 Master 当前帧 SensorData 换算 pitch（与 SensorData.pitch 一致，直接复用）
        float master_pitch = data.pitch;

        // 从最新 Slave HandFrame 换算 pitch
        float slave_pitch = HandFrameToPitch(g_slave_frame);

        BimanualInput bi_input;
        bi_input.master_pitch      = master_pitch;
        bi_input.slave_pitch       = slave_pitch;
        bi_input.slave_frame_age_ms = slave_age_ms;

        BimanualGestureResult bi_result = g_bimanual_recognizer.recognize(bi_input);

        // 每 50 次循环打印一次双手调试信息
        static uint8_t s_bi_debug_counter = 0;
        if (++s_bi_debug_counter >= 50) {
            s_bi_debug_counter = 0;
            DEBUG_LOG("[双手] master_pitch=%.1f  slave_pitch=%.1f  age=%lums  rx=%u",
                      (double)master_pitch,
                      (double)slave_pitch,
                      (unsigned long)slave_age_ms,
                      (unsigned)GetEspNowRxCount());
        }

        if (bi_result.type != BIMANUAL_GESTURE_NONE &&
            now - g_lastAnnounceTime > (unsigned long)TTS_COOLDOWN_MS) {

            DEBUG_LOG("\n[双手识别] 检测到: %s (置信度: %.2f)",
                      bi_result.text, (double)bi_result.confidence);

            // 双手识别直接走 TTS，不经过 LLM 改写（词义已足够完整）
            bool spoken = speak(bi_result.text);
            if (!spoken) {
                if (PlayOfflineVoice(bi_result.text)) {
                    spoken = true;
                }
            }
            if (!spoken) {
                DEBUG_PRINTLN("[系统] 双手手势 TTS 全部失败");
            }

            // 使用与单手相同的冷却计时，避免重复播报
            g_lastAnnounceTime = now;
            // 重置单手手势复位状态，防止双手触发后立即触发同一单手手势
            g_lastAnnouncedGesture = GESTURE_NONE;
        }
    }
#endif  // ENABLE_ESPNOW_SYNC && ESPNOW_ROLE==0
}

// ============================================================
// 辅助函数
// ============================================================

static void printBanner() {
    DEBUG_PRINTLN("\n============================================");
    DEBUG_PRINTLN("  LingxiGlove 灵犀手套");
    DEBUG_PRINTLN("  智能手语翻译系统 - MVP 验证版");
    DEBUG_PRINTLN("============================================");
}

static void haltWithError() {
    DEBUG_PRINTLN("\n[系统] 遇到致命错误，系统停止运行。");
    DEBUG_PRINTLN("  请检查硬件接线后重启。");
    while (1) {
        delay(1000);
    }
}

// ============================================================
// 串口命令交互
// ============================================================
static void printHelp() {
    DEBUG_PRINTLN("  [串口命令] r=识别模式  c=词级采集  f=指拼采集  k=个体校准");
    DEBUG_PRINTLN("             t <文本>=手动触发 Qwen-TTS 播报（脱离手势流验证 TTS 链路）");
    DEBUG_PRINTLN("             l <手势序列>=LLM 改写为自然句后再 TTS 播报");
    DEBUG_PRINTLN("             (序列可用逗号/空格分隔，如: l 我,吃饭  或  l 你好)");
    DEBUG_PRINTLN("             h=帮助");
}

/**
 * @brief 读取串口一整行文本到 out_buf，遇到 \r/\n 终止；带总超时避免死等。
 *
 * 用于 't <text>' 这种"命令字 + 空格 + 变长文本"的交互。返回后 out_buf 已是
 * 以 '\0' 结尾的 C 字符串，首尾空白已去除。超时或 buffer 满都返回 false/截断。
 *
 * @param out_buf    输出缓冲（必须非空）
 * @param buf_size   输出缓冲容量（含 '\0'）
 * @param timeout_ms 整体超时；典型 5000 ms，留给用户粘贴长文本
 * @return true 成功读到非空一行；false 超时或行空
 */
static bool readSerialLine(char* out_buf, size_t buf_size,
                           unsigned long timeout_ms) {
    if (out_buf == nullptr || buf_size < 2) return false;
    size_t used = 0;
    const unsigned long start_ms = millis();
    bool started = false;
    while (millis() - start_ms < timeout_ms) {
        while (Serial.available() > 0) {
            int ch = Serial.read();
            if (ch < 0) break;
            // 把命令字与文本之间的前导空白吃掉，防止误当文本前缀
            if (!started && (ch == ' ' || ch == '\t')) continue;
            if (ch == '\r' || ch == '\n') {
                if (!started) continue;  // 前导空行忽略
                out_buf[used] = '\0';
                while (used > 0 && (out_buf[used - 1] == ' ' ||
                                    out_buf[used - 1] == '\t')) {
                    out_buf[--used] = '\0';
                }
                return used > 0;
            }
            if (used + 1 >= buf_size) {
                DEBUG_PRINTLN("[命令] 输入过长，截断");
                out_buf[used] = '\0';
                return used > 0;
            }
            out_buf[used++] = (char)ch;
            started = true;
        }
        delay(5);
    }
    if (started) {
        out_buf[used] = '\0';
        return used > 0;
    }
    return false;
}

static void handleSerialCommand() {
    if (Serial.available() <= 0) return;

    int ch = Serial.read();
    switch (ch) {
        case 'c':
        case 'C':
            if (g_runMode != MODE_CAPTURE) {
                g_runMode = MODE_CAPTURE;
                g_motionDetector.Reset();
                DEBUG_PRINTLN("\n[模式] 进入词级采集模式（CSV 流），识别与 TTS 已暂停");
                DEBUG_PRINTLN("[模式] 按 r 回到识别模式");
                printCsvHeader();
            }
            break;
        case 'f':
        case 'F':
            if (g_runMode != MODE_FINGER_SPELLING) {
                g_runMode = MODE_FINGER_SPELLING;
                g_motionDetector.Reset();
                DEBUG_PRINTLN("\n[模式] 进入指拼采集模式（CSV 流），识别与 TTS 已暂停");
                DEBUG_PRINTLN("[模式] 说明: 指拼识别模型尚未训练，该模式当前仅做原始数据采集；");
                DEBUG_PRINTLN("       作为未来「开放词汇兜底通道」的接入点。按 r 回到识别模式");
                printCsvHeader();
            }
            break;
        case 'r':
        case 'R':
            if (g_runMode != MODE_RECOGNIZE) {
                g_runMode = MODE_RECOGNIZE;
                g_lastAnnouncedGesture = GESTURE_NONE;
                g_motionDetector.Reset();
                DEBUG_PRINTLN("\n[模式] 恢复识别模式");
            }
            break;
        case 'k':
        case 'K':
            if (g_runMode != MODE_RECOGNIZE) {
                DEBUG_PRINTLN("\n[校准] 请先按 r 回到识别模式再执行校准，避免污染采集数据流");
                break;
            }
            runCalibrationFlow();
            break;
        case 't':
        case 'T': {
            // 手动触发 TTS：'t <text>' 读一整行文本喂给 speak()，用于在不依赖
            // 手势流的前提下单独验证 Qwen-TTS 链路（鉴权 / 下载 / I2S 播放）。
            // 首字节 't' 已被消费，紧随其后可以是 ' text\n' 或 '\ntext\n'
            // （部分串口工具以 Enter 单独结尾）。readSerialLine 已处理首空白与
            // 空行容错，直接读即可。
            char text_buf[256];
            DEBUG_PRINTLN("\n[TTS] 请在 5 秒内输入要播报的文本并回车：");
            if (!readSerialLine(text_buf, sizeof(text_buf), 5000UL)) {
                DEBUG_PRINTLN("[TTS] 未读到有效文本，取消本次播报");
                break;
            }
            DEBUG_LOG("[TTS] 手动触发播报: %s", text_buf);
            bool ok = speak(text_buf);
            if (!ok) {
                DEBUG_PRINTLN("[TTS] 云端失败，尝试离线兜底 ...");
                if (!PlayOfflineVoice(text_buf)) {
                    DEBUG_PRINTLN("[TTS] 离线兜底也未命中，本次无声音输出");
                }
            } else {
                DEBUG_PRINTLN("[TTS] 云端播报完成");
            }
            break;
        }
        case 'l':
        case 'L': {
            // 手动触发 LLM 改写 + TTS：'l <sequence>' 读一整行"手势序列"，
            // 交给 rewriteGestureToSentence 改写成自然句，再喂 speak()。
            // 设计目的：脱离真实手势识别流，独立验证 LLM 改写 + TTS 两段链路，
            // 便于调试 prompt / 模型版本 / 失败回落行为。
            // 输入示例：
            //   l 你好              → LLM: "你好呀"
            //   l 我,吃饭           → LLM: "我想吃饭"
            //   l H,E,L,L,O         → LLM: "你好"
            char seq_buf[256];
            DEBUG_PRINTLN("\n[LLM] 请在 5 秒内输入手势序列并回车（逗号/空格分隔）：");
            if (!readSerialLine(seq_buf, sizeof(seq_buf), 5000UL)) {
                DEBUG_PRINTLN("[LLM] 未读到有效序列，取消本次改写");
                break;
            }
            DEBUG_LOG("[LLM] 手势序列: %s", seq_buf);

            if (WiFi.status() != WL_CONNECTED) {
                DEBUG_PRINTLN("[LLM] WiFi 未就绪，跳过改写，直接按原序列喂 TTS");
                bool ok = speak(seq_buf);
                if (!ok) {
                    DEBUG_PRINTLN("[LLM] 云端 TTS 失败，尝试离线兜底 ...");
                    if (!PlayOfflineVoice(seq_buf)) {
                        DEBUG_PRINTLN("[LLM] 离线兜底也未命中，本次无声音输出");
                    }
                }
                break;
            }

            String sentence = rewriteGestureToSentence(seq_buf);
            const char* to_speak = seq_buf;
            if (sentence.length() > 0) {
                DEBUG_LOG("[LLM] 改写为: %s", sentence);
                to_speak = sentence.c_str();
            } else {
                DEBUG_PRINTLN("[LLM] 改写失败，回落原序列喂 TTS");
            }

            bool ok = speak(to_speak);
            if (!ok) {
                DEBUG_PRINTLN("[LLM] 云端 TTS 失败，尝试离线兜底 ...");
                // 离线兜底仍按原序列匹配（离线表是按手势词建的）
                if (!PlayOfflineVoice(seq_buf)) {
                    DEBUG_PRINTLN("[LLM] 离线兜底也未命中，本次无声音输出");
                }
            } else {
                DEBUG_PRINTLN("[LLM] LLM 改写 + TTS 播报完成");
            }
            break;
        }
        case 'h':
        case 'H':
        case '?':
            printHelp();
            break;
        case '\r':
        case '\n':
        case ' ':
            // 忽略空白符
            break;
        default:
            DEBUG_LOG("[模式] 未知命令: '%c'，按 h 查看帮助", (char)ch);
            break;
    }
}

// ============================================================
// 校准流程（同步阻塞；全程占用主循环）
// ------------------------------------------------------------
// IMU 零偏：要求手套平放静止 ~3s，accel/gyro 采样均值作为零偏
// Flex 量程：ENABLE_FLEX_SENSORS=1 时额外采两阶段（伸直 3s、握拳 3s）
// ============================================================
static void runCalibrationFlow() {
    DEBUG_PRINTLN("\n============================================");
    DEBUG_PRINTLN("  [校准] 开始个体校准");
    DEBUG_PRINTLN("============================================");

    // --- IMU 零偏 ---
    // 关键：先把 sensor_manager 的偏移清零，保证采样回调拿到的是裸物理值。
    // 否则在"第二次及以后"重校准时，readSensors 会先减旧偏移再返回，
    // 校准均值会变成"残差"而非真实偏移，叠加后会出现偏移累积的 bug。
    setImuBias(0, 0, 0, 0, 0, 0);

    DEBUG_PRINTLN("[校准] 步骤 1/1: IMU 零偏");
    DEBUG_PRINTLN("        请把手套【平放】在桌面，保持静止；3 秒后开始采样，采样 3 秒");
    // 倒计时让用户完成摆放
    for (int i = 3; i > 0; i--) {
        DEBUG_LOG("        %d ...", i);
        delay(1000);
    }
    DEBUG_PRINTLN("[校准] 采样中，请勿晃动 ...");
    bool imu_ok = RunImuZeroingCalibration(&g_cal, readSampleAdapter,
                                           3000 /* duration_ms */,
                                           50   /* interval_ms (~20Hz) */);
    if (!imu_ok) {
        DEBUG_PRINTLN("[校准] IMU 零偏采样失败，放弃本次校准");
        return;
    }

#if ENABLE_FLEX_SENSORS
    // --- Flex 量程：伸直阶段 ---
    DEBUG_PRINTLN("[校准] 步骤 2/3: 弯曲传感器 min（手指完全【伸直】）");
    DEBUG_PRINTLN("        请五指完全伸直并保持，3 秒后开始采样 3 秒");
    for (int i = 3; i > 0; i--) {
        DEBUG_LOG("        %d ...", i);
        delay(1000);
    }
    DEBUG_PRINTLN("[校准] 采样中 (min) ...");
    uint16_t flex_min_vals[FLEX_CHANNEL_COUNT];
    if (!RunFlexStageCalibration(flex_min_vals, readFlexRawAdapter, 3000, 50)) {
        DEBUG_PRINTLN("[校准] Flex min 采样失败，保留已完成的 IMU 校准");
        // IMU 部分仍然保存
        SaveCalibration(g_cal);
        ApplyCalibration(g_cal);
        PrintCalibration(g_cal);
        return;
    }

    // --- Flex 量程：握拳阶段 ---
    DEBUG_PRINTLN("[校准] 步骤 3/3: 弯曲传感器 max（手指完全【握拳】）");
    DEBUG_PRINTLN("        请五指完全弯曲握拳并保持，3 秒后开始采样 3 秒");
    for (int i = 3; i > 0; i--) {
        DEBUG_LOG("        %d ...", i);
        delay(1000);
    }
    DEBUG_PRINTLN("[校准] 采样中 (max) ...");
    uint16_t flex_max_vals[FLEX_CHANNEL_COUNT];
    if (!RunFlexStageCalibration(flex_max_vals, readFlexRawAdapter, 3000, 50)) {
        DEBUG_PRINTLN("[校准] Flex max 采样失败，保留已完成的 IMU 校准");
        SaveCalibration(g_cal);
        ApplyCalibration(g_cal);
        PrintCalibration(g_cal);
        return;
    }

    // 校验：每一路 max 必须显著大于 min，否则视为用户操作错误（手指未真正屈伸）
    bool flex_sane = true;
    for (uint8_t i = 0; i < FLEX_CHANNEL_COUNT; i++) {
        if ((uint32_t)flex_max_vals[i] <= (uint32_t)flex_min_vals[i] + 32u) {
            DEBUG_LOG("[校准] Flex 通道 %d 量程过小 (min=%u, max=%u)，请检查手指是否真的屈伸到位", (int)i, (uint32_t)flex_min_vals[i], (uint32_t)flex_max_vals[i]);
            flex_sane = false;
        }
    }
    if (flex_sane) {
        for (uint8_t i = 0; i < FLEX_CHANNEL_COUNT; i++) {
            g_cal.flex_min[i] = flex_min_vals[i];
            g_cal.flex_max[i] = flex_max_vals[i];
        }
        g_cal.flags |= CAL_FLAG_FLEX_OK;
    } else {
        DEBUG_PRINTLN("[校准] Flex 校准整体作废，仅保留 IMU 部分");
    }
#endif  // ENABLE_FLEX_SENSORS

    // --- 写 NVS + 生效 ---
    if (SaveCalibration(g_cal)) {
        DEBUG_PRINTLN("[校准] 已写入 NVS");
    } else {
        DEBUG_PRINTLN("[校准] 警告: NVS 写入部分失败，但本次运行的内存态已生效");
    }
    ApplyCalibration(g_cal);
    PrintCalibration(g_cal);
    DEBUG_PRINTLN("[校准] 完成，回到识别模式\n");

    // 复位防抖 / 运动状态，避免带着校准前的残留直接触发识别
    g_lastAnnouncedGesture = GESTURE_NONE;
    g_motionDetector.Reset();
}

// ============================================================
// 校准流程用的传感器采样适配器：
//   为 calibration 模块提供 ReadSampleFn/ReadFlexRawFn 回调实现，
//   把 SensorData 里的相应字段拆成纯基础类型，避免 calibration.cpp
//   反向依赖 sensor_manager.h。
// ============================================================
static bool readSampleAdapter(float* ax, float* ay, float* az,
                              float* gx, float* gy, float* gz) {
    if (!ax || !ay || !az || !gx || !gy || !gz) return false;
    SensorData data;
    if (!readSensors(data)) return false;
    // 前置条件：调用方 runCalibrationFlow 在进入 IMU 采样前已调用
    // setImuBias(0,...)，因此此处 data.accelX/Y/Z、gyroX/Y/Z 等同于裸物理值。
    // 校准均值直接作为新的偏移写回 g_cal，再由 ApplyCalibration 统一生效。
    *ax = data.accelX;
    *ay = data.accelY;
    *az = data.accelZ;
    *gx = data.gyroX;
    *gy = data.gyroY;
    *gz = data.gyroZ;
    return true;
}

#if ENABLE_FLEX_SENSORS
static bool readFlexRawAdapter(uint16_t out_flex[FLEX_CHANNEL_COUNT]) {
    if (!out_flex) return false;
    SensorData data;
    if (!readSensors(data)) return false;
    if (!data.flexValid) return false;
    for (uint8_t i = 0; i < FLEX_CHANNEL_COUNT; i++) {
        out_flex[i] = data.flex[i];
    }
    return true;
}
#endif

// ============================================================
// CSV 输出（采集模式）
//   固定列: timestamp_ms,ax,ay,az,gx,gy,gz,pitch,roll
//   若 ENABLE_FLEX_SENSORS=1, 追加: flex0,flex1,flex2,flex3,flex4
// ============================================================
static void printCsvHeader() {
    Serial.print("timestamp_ms,ax,ay,az,gx,gy,gz,pitch,roll");
#if ENABLE_FLEX_SENSORS
    for (uint8_t ch = 0; ch < FLEX_CHANNEL_COUNT; ch++) {
        Serial.print(",flex");
        Serial.print(ch);
    }
#endif
    Serial.println();
}

static void printCsvRow(const SensorData& data) {
    Serial.print(data.timestamp); Serial.print(',');
    Serial.print(data.accelX, 4); Serial.print(',');
    Serial.print(data.accelY, 4); Serial.print(',');
    Serial.print(data.accelZ, 4); Serial.print(',');
    Serial.print(data.gyroX, 3);  Serial.print(',');
    Serial.print(data.gyroY, 3);  Serial.print(',');
    Serial.print(data.gyroZ, 3);  Serial.print(',');
    Serial.print(data.pitch, 2);  Serial.print(',');
    Serial.print(data.roll, 2);
#if ENABLE_FLEX_SENSORS
    if (data.flexValid) {
        for (uint8_t ch = 0; ch < FLEX_CHANNEL_COUNT; ch++) {
            Serial.print(',');
            Serial.print(data.flex[ch]);
        }
    } else {
        // 占位，确保列数始终一致
        for (uint8_t ch = 0; ch < FLEX_CHANNEL_COUNT; ch++) {
            Serial.print(",0");
        }
    }
#endif
    Serial.println();
}
