// ============================================================
// config.h — host-side stub for test_bimanual_recognizer
// ------------------------------------------------------------
// 真实 LingxiGlove_Main/config.h 的 DEBUG_LOG 宏依赖 Serial / snprintf / millis，
// 在 PC 上无法编译。本 stub 用同样的 guard 名 CONFIG_H 占位，配合 Makefile
// 的 `-include` 强制最早注入；真实 config.h 在被链式 include 时由于 guard
// 已定义而整个跳过。
//
// **同步点**：以下 BIMANUAL_* / GESTURE_STABLE_MS / FLEX_CHANNEL_COUNT
// 必须与 LingxiGlove_Main/config.h 保持一致；阈值调整时请同步本文件。
// ============================================================

#ifndef CONFIG_H
#define CONFIG_H

// --- 弯曲传感器（让 sensor_manager.h stub 编译通过） ---
#define FLEX_CHANNEL_COUNT      5
#define ENABLE_FLEX_SENSORS     0

// --- 单手识别防抖（RuleBasedRecognizer 用到，本测试不直接覆盖） ---
#define GESTURE_STABLE_MS       500

// --- 双手识别阈值（必须与 LingxiGlove_Main/config.h 一致） ---
#define BIMANUAL_SLAVE_STALE_MS            200
#define BIMANUAL_PITCH_THRESHOLD_DEG       30.0f
#define BIMANUAL_PITCH_DOWN_THRESHOLD_DEG  (-30.0f)
#define BIMANUAL_ROLL_THRESHOLD_DEG        30.0f
#define BIMANUAL_STABLE_MS                 500
#define BIMANUAL_HELP_SLAVE_PITCH_DEG      30.0f
#define BIMANUAL_HELP_MASTER_NEUTRAL_DEG   20.0f

// --- 调试日志：测试中保持安静，避免污染 PASS/FAIL 输出 ---
#define DEBUG_PRINT(...)    ((void)0)
#define DEBUG_PRINTLN(...)  ((void)0)
#define DEBUG_LOG(...)      ((void)0)

#endif  // CONFIG_H
