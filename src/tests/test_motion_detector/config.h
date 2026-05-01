// ============================================================
// test-only config.h stub
// ------------------------------------------------------------
// 用途：host-side 单元测试时，motion_detector.{h,cpp} 会 #include "config.h"。
// 该文件原本是 Arduino 特定的（含 WiFi、I2S 引脚等定义），PC 上无法解析。
// 本 stub **只提供 MotionDetector 所需的 MOTION_* 宏**，值与真实 config.h
// 保持一致（同步点：LingxiGlove_Main/config.h "动作/静止门控" 节）。
//
// 若真实 config.h 中的 MOTION_* 阈值调整，请同步此文件；
// 两者不一致会让测试结果与板上行为脱节。
// ============================================================

#ifndef TEST_CONFIG_H
#define TEST_CONFIG_H

// --- 动作/静止门控（必须与 LingxiGlove_Main/config.h 保持一致） ---
#define ENABLE_MOTION_GATING    1
#define MOTION_WIN_SIZE         10
#define MOTION_WIN_MAX          32
#define MOTION_VAR_ENTER        0.005f
#define MOTION_VAR_EXIT         0.002f
#define MOTION_GYRO_ENTER       15.0f
#define MOTION_GYRO_EXIT        5.0f
#define MOTION_STILL_HOLD_FRAMES 6

#endif  // TEST_CONFIG_H
