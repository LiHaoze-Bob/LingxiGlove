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
#include "nvs_config.h"
#include "accuracy_test.h"
#include "gesture_arbitrator.h"

// LLM 客户端：除 ENABLE_LLM_TEST 启动自检外，主循环里的 LLM 改写层
// （rewriteGestureToSentence）与串口命令 'l' 都需要它，统一无条件 include。
#include "llm_client.h"

// WiFi MAC 地址查询
#include <WiFi.h>

// math.h 用于 BimanualInput 的 pitch 换算（atan2f）
#include <math.h>

// ------------------- 运行模式 -------------------
enum RunMode {
    MODE_RECOGNIZE       = 0,   // 正常识别 + TTS 播报
    MODE_CAPTURE         = 1,   // 数据采集：CSV 输出用于 Edge Impulse 训练（词级手势）
    MODE_FINGER_SPELLING = 2,   // 指拼数据采集：CSV 输出，为后续指拼字母表模型预留
                                // 当前阶段识别器不处理此模式，避免用姿态角规则伪造字母识别结果；
                                // 待指拼模型训练完成后，在此模式下接入专用识别器
    MODE_ACCURACY_TEST   = 3    // 准确率测试：跑识别器但不走 TTS/LLM，每次结果记 CSV 到 LittleFS
    // 说明：校准是一次性阻塞流程（~3s），设计上放在 handleSerialCommand 内同步执行，
    // 不引入独立的 MODE_CALIBRATING 状态（loop() 此时天然被阻塞在 runCalibrationFlow 里）
};

// ------------------- 前置声明 -------------------
static void printBanner();
static void haltWithError();
static void printHelp();
static void handleMultiCharCommand(char first_char);
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
static GestureArbitrator  g_arbitrator;  // 手势仲裁层（方案 C：统一决策）

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
// 运行时角色（NVS 优先 > 编译期 ESPNOW_ROLE 宏）
// ============================================================
// setup() 中通过 LoadNvsRole() 决定实际角色，存入此变量。
// 0 = MASTER, 1 = SLAVE。编译期宏 ESPNOW_ROLE 仅作默认值。
static uint8_t g_runtime_role = ESPNOW_ROLE;

// 对端 MAC（NVS 加载）；全零表示未配置
static uint8_t g_peer_mac[6] = {0};
static bool    g_peer_mac_valid = false;

// ============================================================
// 运行时 WiFi 凭据（NVS 优先 > secrets.h 编译期宏）
// ============================================================
static char g_wifi_ssid[33]     = {0};  // WiFi SSID（最长 32 字符）
static char g_wifi_password[65] = {0};  // WiFi 密码（最长 64 字符）

// ============================================================
// ESP-NOW 双手协同状态（ENABLE_ESPNOW_SYNC=1 时生效）
// ============================================================
// 注意：以下变量无论编译期角色如何都需要编译进来，因为运行时角色可能
// 通过 NVS 切换。SLAVE 固件运行时这些变量不会被使用，仅占少量 RAM。
#if ENABLE_ESPNOW_SYNC

// 最新收到的 Slave 帧及其接收时刻（由 ESP-NOW 接收回调写入，主循环读取）
// 注意：ESP-NOW 回调在系统任务上下文中执行，与 loop() 并发。
// 此处用 volatile 修饰接收时刻，防止编译器优化掉对 g_slave_frame_rx_ms 的读取。
static HandFrame         g_slave_frame;
static volatile uint32_t g_slave_frame_rx_ms = 0;  // 0 表示还未收到任何帧
static bool              g_slave_frame_valid  = false;

// 配对提示：首次收到 Slave 帧时播放升调提示音（仅触发一次）
static bool              g_slave_paired_notified = false;

// 双手规则识别器（在 MASTER 上实例化）
static BimanualRuleRecognizer g_bimanual_recognizer;

/**
 * @brief 将 HandFrame 的 int16 原始 ax/az 换算为 pitch 角（度）
 */
static float HandFrameToPitch(const HandFrame& frame) {
    float ax_g = (float)frame.ax / MPU6050_ACCEL_SCALE_G;
    float az_g = (float)frame.az / MPU6050_ACCEL_SCALE_G;
    return atan2f(-ax_g, az_g) * (180.0f / (float)M_PI);
}

/**
 * @brief 将 HandFrame 的 int16 原始 ay/az 换算为 roll 角（度）
 */
static float HandFrameToRoll(const HandFrame& frame) {
    float ay_g = (float)frame.ay / MPU6050_ACCEL_SCALE_G;
    float az_g = (float)frame.az / MPU6050_ACCEL_SCALE_G;
    return atan2f(ay_g, az_g) * (180.0f / (float)M_PI);
}

/**
 * @brief ESP-NOW 接收回调（系统任务上下文）
 *
 * 严禁在此函数内做任何阻塞操作。仅做帧缓冲写入和时刻记录。
 * LED toggle 使用 GPIO 寄存器直接写，无阻塞，系统任务上下文安全。
 */
static void OnSlaveHandFrame(const HandFrame& frame, const uint8_t /*mac*/[6]) {
    memcpy(&g_slave_frame, &frame, sizeof(HandFrame));
    g_slave_frame_rx_ms = (uint32_t)millis();
    g_slave_frame_valid = true;
#if ESPNOW_LED_INDICATOR
    static bool s_led_lit = false;
    if (!s_led_lit) {
        s_led_lit = true;
        digitalWrite(ESPNOW_LED_PIN, LOW);  // LOW=亮蓝色（低电平有效）
    }
#endif
}

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
// 加载 NVS 运行时角色配置（NVS 优先 > 编译期 ESPNOW_ROLE 宏）
// ============================================================
    {
        uint8_t nvs_role = 0xFF;
        if (LoadNvsRole(&nvs_role)) {
            g_runtime_role = nvs_role;
            DEBUG_LOG("[配置] NVS 角色: %s (覆盖编译期默认值)",
                      g_runtime_role == 0 ? "MASTER" : "SLAVE");
        } else {
            g_runtime_role = ESPNOW_ROLE;
            DEBUG_LOG("[配置] NVS 无角色记录，使用编译期默认: %s",
                      g_runtime_role == 0 ? "MASTER" : "SLAVE");
        }
        // 加载对端 MAC
        if (LoadNvsPeerMac(g_peer_mac)) {
            g_peer_mac_valid = true;
            char mac_str[18];
            FormatMacString(g_peer_mac, mac_str, sizeof(mac_str));
            DEBUG_LOG("[配置] NVS 对端 MAC: %s", mac_str);
        } else {
            g_peer_mac_valid = false;
            DEBUG_PRINTLN("[配置] NVS 无对端 MAC 记录");
        }
        // 打印本机 MAC
        {
            uint8_t self_mac[6];
            WiFi.macAddress(self_mac);
            char self_mac_str[18];
            FormatMacString(self_mac, self_mac_str, sizeof(self_mac_str));
            DEBUG_LOG("[配置] 本机 MAC: %s", self_mac_str);
        }
        // 加载 WiFi 凭据（NVS 优先 > secrets.h 编译期宏）
        if (LoadNvsWifiSsid(g_wifi_ssid, sizeof(g_wifi_ssid)) &&
            LoadNvsWifiPassword(g_wifi_password, sizeof(g_wifi_password))) {
            DEBUG_LOG("[配置] NVS WiFi SSID: %s", g_wifi_ssid);
        } else {
            strncpy(g_wifi_ssid, WIFI_SSID, sizeof(g_wifi_ssid) - 1);
            strncpy(g_wifi_password, WIFI_PASSWORD, sizeof(g_wifi_password) - 1);
            DEBUG_LOG("[配置] 使用编译期 WiFi: %s", g_wifi_ssid);
        }
    }

// ---------- LED 指示灯初始化（MASTER/SLAVE 均需要）----------
// RGB LED (WS2812)：neopixelWrite 不需要 pinMode，直接写即可
#if ESPNOW_LED_INDICATOR && ENABLE_ESPNOW_SYNC
    pinMode(ESPNOW_LED_PIN, OUTPUT);
    digitalWrite(ESPNOW_LED_PIN, HIGH);  // HIGH=灭（低电平有效 RGB LED）
    DEBUG_PRINTLN("[系统] ESP-NOW LED 指示灯已启用（通信成功后亮蓝色）");
#endif

// ============================================================
// SLAVE / MASTER 启动分叉（运行时判断 g_runtime_role）
// ============================================================
#if ENABLE_ESPNOW_SYNC
    if (g_runtime_role == 1) {
        // ---------- SLAVE 启动路径 ----------
        // SLAVE 不需要 I2S / TTS / LLM，但**必须**先连 WiFi AP 同步信道。
        // ESP-NOW 要求两端在同一 WiFi 信道，MASTER 连 AP 后信道由 AP 决定；
        // SLAVE 若不连同一 AP，会停留在默认信道 1，导致帧全部丢失。
        DEBUG_PRINTLN("[Slave] 正在连接 WiFi（同步信道）...");
        bool slave_wifi_ok = connectWiFi(g_wifi_ssid, g_wifi_password, WIFI_TIMEOUT_MS);
        if (slave_wifi_ok) {
            DEBUG_LOG("[Slave] WiFi 已连接，信道同步完成 (IP: %s)",
                      WiFi.localIP().toString().c_str());
        } else {
            DEBUG_PRINTLN("[Slave] 警告: WiFi 连接失败，ESP-NOW 将使用默认信道（可能与 Master 不同）");
        }

        DEBUG_PRINTLN("[Slave] 正在初始化 ESP-NOW...");
        const uint8_t* slave_peer = g_peer_mac_valid ? g_peer_mac : nullptr;
        if (!InitEspNowSync(ESPNOW_ROLE_SLAVE, slave_peer)) {
            DEBUG_PRINTLN("[Slave] 错误: ESP-NOW 初始化失败，系统停止");
            haltWithError();
        }
        if (g_peer_mac_valid) {
            char mac_str[18];
            FormatMacString(g_peer_mac, mac_str, sizeof(mac_str));
            DEBUG_LOG("[Slave] ESP-NOW 初始化成功，定向发送给 %s", mac_str);
        } else {
            DEBUG_PRINTLN("[Slave] ESP-NOW 初始化成功，广播 HandFrame...");
            DEBUG_PRINTLN("[Slave] 提示: 可用 'peer AA:BB:CC:DD:EE:FF' 设置 MASTER 地址");
        }
        // LED 在 loop 中等首次 ACK 成功后再亮蓝色
        g_systemReady = true;
        printHelp();
        return;  // SLAVE setup() 到此结束
    }
#endif  // ENABLE_ESPNOW_SYNC

    // ---------- MASTER 启动路径（含非 ESPNOW 的单手 MVP）----------

    // ---------- 3. 初始化 I2S 音频 ----------
    DEBUG_PRINTLN("[系统] 正在初始化 I2S 音频...");
    if (!initTTS()) {
        DEBUG_PRINTLN("[系统] 警告: I2S 音频初始化失败，语音播报不可用");
        DEBUG_PRINTLN("  预期接线: BCLK->D4/GPIO7, LRC->D5/GPIO8, DIN->D6/GPIO9");
    } else {
        DEBUG_PRINTLN("[系统] I2S 初始化成功，播放开机提示音...");
        playTestTone(880, 200);   // A5
        delay(100);
        playTestTone(1100, 200);  // C#6
    }

    // ---------- 4. 连接 WiFi ----------
    DEBUG_PRINTLN("[系统] 正在连接 WiFi...");
    DEBUG_LOG("[系统] SSID: %s", g_wifi_ssid);
    DEBUG_PRINTLN("[系统] 提示: 连接失败时可输入 'wifi <SSID> <PASSWORD>' 修改凭据");
    bool wifiOk = false;
    while (!wifiOk) {
        wifiOk = connectWiFi(g_wifi_ssid, g_wifi_password, WIFI_TIMEOUT_MS);
        if (!wifiOk) {
            DEBUG_PRINTLN("[系统] WiFi 连接失败，等待 5 秒重试（期间可输入 wifi 命令修改凭据）...");
            // 在等待期间轮询串口，允许用户输入 wifi 命令修改凭据
            unsigned long wait_start = millis();
            while (millis() - wait_start < 5000) {
                if (Serial.available() > 0) {
                    int ch = Serial.read();
                    if (ch == 'w' || ch == 'W') {
                        handleMultiCharCommand((char)ch);
                        // handleMultiCharCommand 中 wifi 命令会自动重启，
                        // 如果用户输入的不是 wifi 命令则继续等待
                    }
                }
                delay(10);
            }
        }
    }
    // WiFi 连上后信道已确定，MASTER 现在才初始化 ESP-NOW，
    // 确保 ESP-NOW 与 WiFi 使用同一信道（ESP-NOW channel=0 表示跟随当前 WiFi 信道）
#if ENABLE_ESPNOW_SYNC
    {
        DEBUG_PRINTLN("[Master] 正在初始化 ESP-NOW...");
        const uint8_t* master_peer = g_peer_mac_valid ? g_peer_mac : nullptr;
        if (InitEspNowSync(ESPNOW_ROLE_MASTER, master_peer)) {
            RegisterHandFrameHandler(OnSlaveHandFrame);
            g_bimanual_recognizer.init();
            g_arbitrator.init();  // 手势仲裁层初始化
            DEBUG_PRINTLN("[系统] 手势仲裁器就绪 (确认200ms, 冷却2000ms)");
            if (g_peer_mac_valid) {
                char mac_str[18];
                FormatMacString(g_peer_mac, mac_str, sizeof(mac_str));
                DEBUG_LOG("[Master] ESP-NOW 初始化成功，等待 Slave(%s) 帧...", mac_str);
            } else {
                DEBUG_PRINTLN("[Master] ESP-NOW 初始化成功（接收模式，未指定 Slave MAC）");
                DEBUG_PRINTLN("[Master] 提示: 可用 'peer AA:BB:CC:DD:EE:FF' 设置 SLAVE 地址");
            }
        } else {
            DEBUG_PRINTLN("[Master] 警告: ESP-NOW 初始化失败，双手识别不可用");
        }
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
    if (isWiFiConnected()) {
        if (!speak("灵犀手套已就绪")) {
            DEBUG_PRINTLN("[系统] 开机 TTS 失败，2s 后重试一次...");
            delay(2000);
            speak("灵犀手套已就绪");
        }
    }
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
        if (g_runtime_role == 0) {
            checkWiFiConnection(g_wifi_ssid, g_wifi_password);
        }
        return;
    }

// ============================================================
// SLAVE / MASTER 主循环分支（运行时判断 g_runtime_role）
// ============================================================
#if ENABLE_ESPNOW_SYNC
    if (g_runtime_role == 1) {
        // ---------- SLAVE 主循环：采样 → 打包 HandFrame → 发出 ----------
        static uint16_t s_slave_seq = 0;
        HandFrame frame;
        memset(&frame, 0, sizeof(frame));
        frame.master_timestamp_ms = 0;
        frame.seq_no              = s_slave_seq++;
        frame.frame_type          = FRAME_TYPE_SENSOR_DATA;
        frame.proto_version       = HANDFRAME_PROTO_VERSION;
        frame.ax = (int16_t)(data.accelX * MPU6050_ACCEL_SCALE_G);
        frame.ay = (int16_t)(data.accelY * MPU6050_ACCEL_SCALE_G);
        frame.az = (int16_t)(data.accelZ * MPU6050_ACCEL_SCALE_G);
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
#if ESPNOW_LED_INDICATOR
            static bool s_slave_led_lit = false;
            if (!s_slave_led_lit && GetEspNowTxCount() > 0) {
                s_slave_led_lit = true;
                digitalWrite(ESPNOW_LED_PIN, LOW);  // 首次 ACK 成功，亮蓝色
                DEBUG_PRINTLN("[Slave] LED 已亮蓝色（Master 已确认收到帧）");
            }
#endif
            static uint16_t s_print_counter = 0;
            if (++s_print_counter >= 50) {
                s_print_counter = 0;
                DEBUG_LOG("[Slave] 已发 %u 帧  pitch=%.1f  roll=%.1f  seq=%u",
                          (unsigned)GetEspNowTxCount(),
                          (double)data.pitch,
                          (double)data.roll,
                          (unsigned)frame.seq_no);
            }
        }
        return;  // SLAVE 不走后续 MASTER 路径
    }
#endif  // ENABLE_ESPNOW_SYNC

    // ---------- MASTER 主循环（含非 ESPNOW 的单手 MVP）----------

    // ESP-NOW 配对提示：首次收到 Slave 帧时播放升调双音，告知用户双手已连通
#if ENABLE_ESPNOW_SYNC
    if (g_slave_frame_valid && !g_slave_paired_notified) {
        g_slave_paired_notified = true;
        DEBUG_PRINTLN("\n[双手] ✅ Slave 已连接！首帧已收到");
        {
            float first_slave_pitch = HandFrameToPitch(g_slave_frame);
            float first_slave_roll  = HandFrameToRoll(g_slave_frame);
            DEBUG_LOG("[双手] Slave seq=%u  rx_count=%u  pitch=%.1f  roll=%.1f",
                      (unsigned)g_slave_frame.seq_no,
                      (unsigned)GetEspNowRxCount(),
                      (double)first_slave_pitch,
                      (double)first_slave_roll);
        }
        // 升调双音：C6(1047Hz) → E6(1319Hz)，清脆明快，区别于开机提示音(A5→C#6)
        playTestTone(1047, 120);
        delay(60);
        playTestTone(1319, 120);
    }
#endif

    if (g_runMode == MODE_CAPTURE || g_runMode == MODE_FINGER_SPELLING) {
        printCsvRow(data);
    } else {
        doRecognizeStep(data, now);
    }

    // ---------- 维护 WiFi 连接 ----------
    checkWiFiConnection(g_wifi_ssid, g_wifi_password);
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

        // 准确率测试模式：STILL 信号需要喂给 accuracy_test 推进 rest 阶段
        if (g_runMode == MODE_ACCURACY_TEST && IsAccuracyTestActive()) {
            TickAccuracyTest(now, ACC_SOURCE_SINGLE_HAND, nullptr, 0.0f,
                             data.pitch, data.roll, true);
            TickAccuracyTest(now, ACC_SOURCE_BIMANUAL, nullptr, 0.0f,
                             data.pitch, data.roll, true);
        }

#if ENABLE_ESPNOW_SYNC
        // 静止时也打印双手数据，便于调试通信是否正常
        if (g_runtime_role == 0 && g_slave_frame_valid) {
            static uint8_t s_still_debug_counter = 0;
            if (++s_still_debug_counter >= 50) {
                s_still_debug_counter = 0;
                float slave_pitch = HandFrameToPitch(g_slave_frame);
                float slave_roll  = HandFrameToRoll(g_slave_frame);
                unsigned long slave_age_ms = (now >= g_slave_frame_rx_ms)
                    ? (now - g_slave_frame_rx_ms) : 0;
                DEBUG_LOG("[双手] mP=%.1f sP=%.1f mR=%.1f sR=%.1f age=%lums rx=%u (STILL)",
                          (double)data.pitch, (double)slave_pitch,
                          (double)data.roll, (double)slave_roll,
                          (unsigned long)slave_age_ms,
                          (unsigned)GetEspNowRxCount());
            }
        }
#endif
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

    // 准确率测试模式：记录识别结果，跳过 TTS/LLM
    if (g_runMode == MODE_ACCURACY_TEST && IsAccuracyTestActive()) {
        const char* det = (result.type != GESTURE_NONE) ? result.text : nullptr;
        TickAccuracyTest(now, ACC_SOURCE_SINGLE_HAND, det, result.confidence,
                         data.pitch, data.roll, false /* MOVING 分支必非 still */);
    }

    // ---- 仲裁层候选准备（方案 C：统一决策） ----
    // 单手/双手候选不再自行播报，统一交给仲裁器决策。
    GestureCandidate single_candidate = { GESTURE_SOURCE_NONE, "", 0.0f };
    if (result.type != GESTURE_NONE) {
        single_candidate.source     = GESTURE_SOURCE_SINGLE_HAND;
        single_candidate.text       = result.text;
        single_candidate.confidence = result.confidence;
    }

    // 手势复位：当回到无手势状态时，允许再次识别同一手势
    if (result.type == GESTURE_NONE) {
        g_lastAnnouncedGesture = GESTURE_NONE;
    }

// ============================================================
// Master 双手协同识别（运行时判断 g_runtime_role == 0）
// 候选不再自行播报，统一交给仲裁器决策。
// ============================================================
    GestureCandidate bimanual_candidate = { GESTURE_SOURCE_NONE, "", 0.0f };

#if ENABLE_ESPNOW_SYNC
    if (g_runtime_role == 0) {
        unsigned long slave_age_ms = 0;
        if (g_slave_frame_valid) {
            uint32_t rx_ms = g_slave_frame_rx_ms;
            slave_age_ms = (now >= rx_ms) ? (now - rx_ms) : 0;
        } else {
            slave_age_ms = BIMANUAL_SLAVE_STALE_MS + 1;
        }

        float master_pitch = data.pitch;
        float master_roll  = data.roll;
        float slave_pitch  = HandFrameToPitch(g_slave_frame);
        float slave_roll   = HandFrameToRoll(g_slave_frame);

        BimanualInput bi_input;
        bi_input.master_pitch       = master_pitch;
        bi_input.slave_pitch        = slave_pitch;
        bi_input.master_roll        = master_roll;
        bi_input.slave_roll         = slave_roll;
        bi_input.slave_frame_age_ms = slave_age_ms;

        BimanualGestureResult bi_result = g_bimanual_recognizer.recognize(bi_input);

        static uint8_t s_bi_debug_counter = 0;
        if (++s_bi_debug_counter >= 50) {
            s_bi_debug_counter = 0;
            DEBUG_LOG("[双手] mP=%.1f sP=%.1f mR=%.1f sR=%.1f age=%lums rx=%u",
                      (double)master_pitch, (double)slave_pitch,
                      (double)master_roll, (double)slave_roll,
                      (unsigned long)slave_age_ms,
                      (unsigned)GetEspNowRxCount());
        }

        // 准确率测试模式：记录双手识别结果，跳过 TTS
        if (g_runMode == MODE_ACCURACY_TEST && IsAccuracyTestActive()) {
            const char* bi_det = (bi_result.type != BIMANUAL_GESTURE_NONE)
                                 ? bi_result.text : nullptr;
            TickAccuracyTest(now, ACC_SOURCE_BIMANUAL, bi_det, bi_result.confidence,
                             data.pitch, data.roll, false);
        }

        // ---- 仲裁层候选准备（方案 C） ----
        if (bi_result.type != BIMANUAL_GESTURE_NONE) {
            bimanual_candidate.source     = GESTURE_SOURCE_BIMANUAL;
            bimanual_candidate.text       = bi_result.text;
            bimanual_candidate.confidence = bi_result.confidence;
        }
    }
#endif  // ENABLE_ESPNOW_SYNC

// ============================================================
// 手势仲裁层：统一决策单手/双手播报（方案 C）
// 准确率测试模式跳过仲裁，直接记录识别结果。
// ============================================================
    if (g_runMode != MODE_ACCURACY_TEST) {
        ArbitratedGesture arb = g_arbitrator.tick(single_candidate, bimanual_candidate, now);
        if (arb.should_announce) {
            DEBUG_LOG("\n[仲裁] 播报: '%s' (来源=%s, 置信度=%.2f)",
                      arb.text,
                      arb.source == GESTURE_SOURCE_BIMANUAL ? "双手" : "单手",
                      (double)arb.confidence);

            // TTS 播报：单手支持 LLM 改写 + 缓存，双手仅在线/离线 TTS
            bool spoken = false;
            if (arb.source == GESTURE_SOURCE_SINGLE_HAND) {
                // ----- 快速路径：TTS 缓存命中 -----
                spoken = speak(arb.text, arb.text, true);
                if (spoken) {
                    DEBUG_PRINTLN("[仲裁] TTS 缓存命中，跳过 LLM 改写");
                }
                // ----- 慢速路径：LLM 改写 + 云端 TTS -----
                if (!spoken) {
                    const char* spoken_text = arb.text;
#if ENABLE_LLM_REWRITE
                    String rewritten;
                    if (WiFi.status() == WL_CONNECTED) {
                        rewritten = rewriteGestureToSentence(arb.text);
                        if (rewritten.length() > 0) {
                            DEBUG_LOG("[仲裁] LLM 改写: %s", rewritten.c_str());
                            spoken_text = rewritten.c_str();
                        }
                    }
#endif
                    spoken = speak(spoken_text, arb.text);
                }
            } else {
                // 双手手势：直接在线 TTS（无缓存/LLM）
                spoken = speak(arb.text);
            }

            // 离线兜底
            if (!spoken) {
                spoken = PlayOfflineVoice(arb.text);
            }
            if (!spoken) {
                DEBUG_PRINTLN("[仲裁] 全部播报通道失败");
            }

            g_lastAnnouncedGesture = GESTURE_NONE;  // 允许仲裁器决定下次播报
            g_lastAnnounceTime = now;
        }
    }
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
    DEBUG_PRINTLN("             t <文本>=手动触发 Qwen-TTS 播报");
    DEBUG_PRINTLN("             l <手势序列>=LLM 改写 + TTS 播报");
    DEBUG_PRINTLN("             test help — 准确率测试命令族（id 表 + 用法）");
    DEBUG_PRINTLN("             i=查看当前配置(角色/MAC/WiFi)");
    DEBUG_PRINTLN("             role master|slave  — 设置角色并重启");
    DEBUG_PRINTLN("             peer AA:BB:CC:DD:EE:FF — 设置对端 MAC 并重启");
    DEBUG_PRINTLN("             wifi <SSID> <PASSWORD> — 设置 WiFi 并重启");
    DEBUG_PRINTLN("             wifi clear — 清除 WiFi 配置恢复默认");
    DEBUG_PRINTLN("             nvs clear — 清除 NVS 配置并重启");
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

/**
 * @brief 打印当前配置信息：角色、本机 MAC、对端 MAC
 */
static void printDeviceInfo() {
    DEBUG_PRINTLN("\n============ 设备信息 ============");
    DEBUG_LOG("  角色:     %s (NVS %s)",
              g_runtime_role == 0 ? "MASTER" : "SLAVE",
              g_peer_mac_valid ? "已配置" : "未配置对端");
    DEBUG_LOG("  编译默认: %s", ESPNOW_ROLE == 0 ? "MASTER" : "SLAVE");

    uint8_t self_mac[6];
    WiFi.macAddress(self_mac);
    char self_mac_str[18];
    FormatMacString(self_mac, self_mac_str, sizeof(self_mac_str));
    DEBUG_LOG("  本机 MAC: %s", self_mac_str);

    if (g_peer_mac_valid) {
        char peer_mac_str[18];
        FormatMacString(g_peer_mac, peer_mac_str, sizeof(peer_mac_str));
        DEBUG_LOG("  对端 MAC: %s", peer_mac_str);
    } else {
        DEBUG_PRINTLN("  对端 MAC: 未设置 (用 'peer AA:BB:CC:DD:EE:FF' 设置)");
    }
    DEBUG_LOG("  WiFi SSID: %s", g_wifi_ssid);
    DEBUG_PRINTLN("==================================");
}

/**
 * @brief 处理多字符串口命令（如 "role master"、"peer AA:BB:CC:DD:EE:FF"）
 *
 * 将首字符 + readSerialLine 读到的后续内容拼成完整命令行，再统一解析。
 * 对于单字符命令（r/c/f/k/h 等），首字符后无后续内容，直接处理。
 *
 * @param first_char 已读取的首字符
 */
static void handleMultiCharCommand(char first_char) {
    // 拼完整命令行：首字符 + 后续行内容
    char cmd_buf[64];
    cmd_buf[0] = first_char;
    cmd_buf[1] = '\0';

    // 短暂等待后续字符（串口工具逐字符到达，给 50ms 缓冲）
    delay(50);
    if (Serial.available() > 0) {
        char rest[62];
        if (readSerialLine(rest, sizeof(rest), 2000UL)) {
            // 拼接：首字符 + rest
            size_t first_len = 1;
            size_t rest_len = strlen(rest);
            if (first_len + rest_len + 1 <= sizeof(cmd_buf)) {
                memcpy(cmd_buf + 1, rest, rest_len + 1);
            }
        }
    }

    // 解析命令
    if (strncmp(cmd_buf, "role", 4) == 0) {
        // "role master" 或 "role slave"
        const char* arg = cmd_buf + 4;
        while (*arg == ' ') arg++;
        if (strncmp(arg, "master", 6) == 0 || strncmp(arg, "MASTER", 6) == 0) {
            DEBUG_PRINTLN("[配置] 设置角色为 MASTER，写入 NVS...");
            if (SaveNvsRole(0)) {
                DEBUG_PRINTLN("[配置] 已保存，3 秒后重启...");
                delay(3000);
                ESP.restart();
            } else {
                DEBUG_PRINTLN("[配置] NVS 写入失败");
            }
        } else if (strncmp(arg, "slave", 5) == 0 || strncmp(arg, "SLAVE", 5) == 0) {
            DEBUG_PRINTLN("[配置] 设置角色为 SLAVE，写入 NVS...");
            if (SaveNvsRole(1)) {
                DEBUG_PRINTLN("[配置] 已保存，3 秒后重启...");
                delay(3000);
                ESP.restart();
            } else {
                DEBUG_PRINTLN("[配置] NVS 写入失败");
            }
        } else {
            DEBUG_PRINTLN("[配置] 用法: role master 或 role slave");
        }
    } else if (strncmp(cmd_buf, "peer", 4) == 0) {
        // "peer AA:BB:CC:DD:EE:FF"
        const char* arg = cmd_buf + 4;
        while (*arg == ' ') arg++;
        uint8_t new_mac[6];
        if (ParseMacString(arg, new_mac)) {
            char mac_str[18];
            FormatMacString(new_mac, mac_str, sizeof(mac_str));
            DEBUG_LOG("[配置] 设置对端 MAC: %s，写入 NVS...", mac_str);
            if (SaveNvsPeerMac(new_mac)) {
                DEBUG_PRINTLN("[配置] 已保存，3 秒后重启...");
                delay(3000);
                ESP.restart();
            } else {
                DEBUG_PRINTLN("[配置] NVS 写入失败");
            }
        } else {
            DEBUG_PRINTLN("[配置] 格式错误，用法: peer AA:BB:CC:DD:EE:FF");
        }
    } else if (strncmp(cmd_buf, "nvs", 3) == 0) {
        // "nvs clear"
        const char* arg = cmd_buf + 3;
        while (*arg == ' ') arg++;
        if (strncmp(arg, "clear", 5) == 0) {
            DEBUG_PRINTLN("[配置] 清除 NVS 角色/MAC 配置...");
            if (ClearNvsConfig()) {
                DEBUG_PRINTLN("[配置] 已清除，恢复编译期默认值，3 秒后重启...");
                delay(3000);
                ESP.restart();
            } else {
                DEBUG_PRINTLN("[配置] NVS 清除失败");
            }
        } else {
            DEBUG_PRINTLN("[配置] 用法: nvs clear");
        }
    } else if (strncmp(cmd_buf, "wifi", 4) == 0) {
        const char* arg = cmd_buf + 4;
        while (*arg == ' ') arg++;
        if (strncmp(arg, "clear", 5) == 0) {
            DEBUG_PRINTLN("[配置] 清除 NVS WiFi 配置...");
            if (ClearNvsWifi()) {
                DEBUG_PRINTLN("[配置] 已清除，恢复编译期默认 WiFi，3 秒后重启...");
                delay(3000);
                ESP.restart();
            } else {
                DEBUG_PRINTLN("[配置] NVS 清除失败");
            }
        } else if (strlen(arg) > 0) {
            // 解析 "wifi <SSID> <PASSWORD>"
            // SSID 中不允许空格（WiFi 规范允许，但此处简化处理）
            char new_ssid[33] = {0};
            char new_pass[65] = {0};
            const char* space_pos = strchr(arg, ' ');
            if (space_pos) {
                size_t ssid_len = (size_t)(space_pos - arg);
                if (ssid_len >= sizeof(new_ssid)) ssid_len = sizeof(new_ssid) - 1;
                memcpy(new_ssid, arg, ssid_len);
                new_ssid[ssid_len] = '\0';
                const char* pass_start = space_pos + 1;
                while (*pass_start == ' ') pass_start++;
                strncpy(new_pass, pass_start, sizeof(new_pass) - 1);
            } else {
                // 仅 SSID，无密码（开放网络）
                strncpy(new_ssid, arg, sizeof(new_ssid) - 1);
            }
            DEBUG_LOG("[配置] 设置 WiFi SSID: %s，写入 NVS...", new_ssid);
            if (SaveNvsWifi(new_ssid, new_pass)) {
                DEBUG_PRINTLN("[配置] 已保存，3 秒后重启...");
                delay(3000);
                ESP.restart();
            } else {
                DEBUG_PRINTLN("[配置] NVS 写入失败");
            }
        } else {
            DEBUG_PRINTLN("[配置] 用法: wifi <SSID> <PASSWORD>");
            DEBUG_PRINTLN("       wifi clear — 清除恢复默认");
            DEBUG_LOG("  当前 SSID: %s", g_wifi_ssid);
        }
    } else if (strncmp(cmd_buf, "info", 4) == 0) {
        printDeviceInfo();
    } else if (strncmp(cmd_buf, "test", 4) == 0) {
        const char* arg = cmd_buf + 4;
        while (*arg == ' ') arg++;

        if (strncmp(arg, "cancel", 6) == 0) {
            CancelAccuracyTest();
            // 取消后回到识别模式
            if (g_runMode == MODE_ACCURACY_TEST) {
                g_runMode = MODE_RECOGNIZE;
                g_lastAnnouncedGesture = GESTURE_NONE;
                g_motionDetector.Reset();
                DEBUG_PRINTLN("[模式] 已退出测试模式，恢复识别模式");
            }
        } else if (strncmp(arg, "export", 6) == 0) {
            if (IsAccuracyTestActive()) {
                DEBUG_PRINTLN("[测试] 当前正在测试中，请先 'test cancel' 或等待完成");
            } else {
                ExportAccuracyTestLogs();
            }
        } else if (strncmp(arg, "clear", 5) == 0) {
            if (IsAccuracyTestActive()) {
                DEBUG_PRINTLN("[测试] 当前正在测试中，请先 'test cancel'");
            } else {
                ClearAccuracyTestLogs();
            }
        } else if (strncmp(arg, "help", 4) == 0 || *arg == '\0') {
            DEBUG_PRINTLN("\n============ 准确率测试 ============");
            DEBUG_PRINTLN("命令:");
            DEBUG_PRINTLN("  test <id> <count>  启动测试，例: test 1 30 → 测「你好」30 次");
            DEBUG_PRINTLN("  test cancel        取消当前会话");
            DEBUG_PRINTLN("  test export        dump 所有历史日志到串口");
            DEBUG_PRINTLN("  test clear         清空所有日志 + 复位会话计数器");
            DEBUG_PRINTLN("  test help          打印本帮助");
            DEBUG_PRINTLN("");
            DEBUG_PRINTLN("手势 ID:");
            DEBUG_PRINTLN("  单手: 1 你好  2 谢谢  3 再见  4 是  5 不");
            DEBUG_PRINTLN("  双手: 101 加油  102 一起  103 我爱你  104 帮助");
            DEBUG_PRINTLN("=====================================");
        } else {
            int id = 0, count = 0;
            if (sscanf(arg, "%d %d", &id, &count) == 2 && id > 0 && count > 0) {
                // 切到测试模式
                if (g_runMode != MODE_ACCURACY_TEST) {
                    g_runMode = MODE_ACCURACY_TEST;
                    g_lastAnnouncedGesture = GESTURE_NONE;
                    g_motionDetector.Reset();
                    DEBUG_PRINTLN("\n[模式] 进入准确率测试模式（TTS/LLM 已暂停）");
                }
                if (!StartAccuracyTest((uint16_t)id, (uint16_t)count)) {
                    g_runMode = MODE_RECOGNIZE;
                    DEBUG_PRINTLN("[模式] 启动失败，恢复识别模式");
                }
            } else {
                DEBUG_LOG("[测试] 用法错误，参数: '%s'。试试 'test help'", arg);
            }
        }
    } else {
        DEBUG_LOG("[模式] 未知命令: '%s'，按 h 查看帮助", cmd_buf);
    }
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
            // peek 下一个字符：若是 'e'，是 'test ...' 命令族；否则是 't <text>' TTS
            delay(30);
            if (Serial.available() > 0) {
                int next = Serial.peek();
                if (next == 'e' || next == 'E') {
                    handleMultiCharCommand((char)ch);
                    break;
                }
            }

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
        case 'i':
        case 'I':
            printDeviceInfo();
            break;
        case '\r':
        case '\n':
        case ' ':
            break;
        default:
            // 对 r/n/p 开头的命令，判断是否为多字符命令
            // r + 无后续 = 恢复识别模式；role/... = 多字符命令
            if (ch == 'r' || ch == 'R') {
                // peek 下一个字符，如果是字母则走多字符命令
                delay(30);
                if (Serial.available() > 0) {
                    int next = Serial.peek();
                    if (next >= 'a' && next <= 'z') {
                        handleMultiCharCommand((char)ch);
                        break;
                    }
                }
                // 单字符 r = 恢复识别模式
                if (g_runMode != MODE_RECOGNIZE) {
                    if (IsAccuracyTestActive()) {
                        CancelAccuracyTest();
                    }
                    g_runMode = MODE_RECOGNIZE;
                    g_lastAnnouncedGesture = GESTURE_NONE;
                    g_motionDetector.Reset();
                    DEBUG_PRINTLN("\n[模式] 恢复识别模式");
                }
            } else if (ch == 'n' || ch == 'N' || ch == 'p' || ch == 'P' ||
                       ch == 'w' || ch == 'W') {
                handleMultiCharCommand((char)ch);
            } else {
                DEBUG_LOG("[模式] 未知命令: '%c'，按 h 查看帮助", (char)ch);
            }
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
