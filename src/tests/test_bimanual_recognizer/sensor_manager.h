// ============================================================
// sensor_manager.h — host-side stub for test_bimanual_recognizer
// ------------------------------------------------------------
// 真实 sensor_manager.h 拉了 MPU6050 / Wire 等 Arduino-only 依赖。
// 本 stub 使用与真实文件相同的 guard 名 SENSOR_MANAGER_H，配合 Makefile
// 的 `-include` 抢先注入，真实头在链式 include 时整个跳过。
//
// 双手识别器用的 BimanualInput 与 SensorData 无关，但同 cpp 的
// RuleBasedRecognizer::recognize(const SensorData&) 需要类型完整定义
// 才能编译——所以这里给一个最小的 SensorData。
// ============================================================

#ifndef SENSOR_MANAGER_H
#define SENSOR_MANAGER_H

#include <Arduino.h>
#include "config.h"   // 提供 FLEX_CHANNEL_COUNT、ENABLE_FLEX_SENSORS

enum FlexFinger {
    FLEX_THUMB = 0,
    FLEX_INDEX = 1,
    FLEX_MIDDLE = 2,
    FLEX_RING = 3,
    FLEX_PINKY = 4
};

struct SensorData {
    float accelX;
    float accelY;
    float accelZ;
    float gyroX;
    float gyroY;
    float gyroZ;
    float pitch;
    float roll;
    uint16_t flex[FLEX_CHANNEL_COUNT];
    float    flexNorm[FLEX_CHANNEL_COUNT];
    bool     flexValid;
    bool     mpuValid;
    unsigned long timestamp;
};

#endif  // SENSOR_MANAGER_H
