// ============================================================
// gesture_recognizer.h
// 手势识别引擎 - 抽象层设计，支持多种实现切换
// ============================================================
// MVP阶段: RuleBasedRecognizer（基于MPU6050姿态角的硬编码规则）
// 全功能阶段: EdgeImpulseRecognizer（替换为Edge Impulse C++模型）
// ============================================================

#ifndef GESTURE_RECOGNIZER_H
#define GESTURE_RECOGNIZER_H

#include <Arduino.h>
#include "sensor_manager.h"

// ------------------- 手势类型枚举 -------------------
enum GestureType {
    GESTURE_NONE = 0,
    GESTURE_HELLO,      // 你好
    GESTURE_THANKS,     // 谢谢
    GESTURE_GOODBYE,    // 再见
    GESTURE_YES,        // 是
    GESTURE_NO,         // 不
    GESTURE_COUNT
};

// ------------------- 手势识别结果 -------------------
struct GestureResult {
    GestureType type;       // 手势类型
    const char* text;       // 对应中文文本
    float confidence;       // 置信度 0.0~1.0
};

// ------------------- 抽象基类 -------------------
class GestureRecognizer {
public:
    virtual ~GestureRecognizer() {}

    // 初始化识别器（加载模型/配置参数）
    virtual bool init() = 0;

    // 根据传感器数据识别手势
    // 返回识别结果，未识别时 type = GESTURE_NONE
    virtual GestureResult recognize(const SensorData& data) = 0;

    // 获取识别器名称（用于日志显示）
    virtual const char* getName() const = 0;
};

// ------------------- MVP阶段：规则识别器 -------------------
// 基于 MPU6050 的 pitch/roll 姿态角做硬编码规则判定
class RuleBasedRecognizer : public GestureRecognizer {
public:
    bool init() override;
    GestureResult recognize(const SensorData& data) override;
    const char* getName() const override { return "RuleBased(MPU6050)"; }

private:
    // 判定阈值（度）
    static constexpr float ANGLE_THRESHOLD = 45.0f;
    static constexpr float NEUTRAL_ZONE    = 20.0f;

    // 防抖相关状态
    GestureType m_lastDetected;     // 上次检测到的手势
    unsigned long m_stableStartMs;  // 当前手势开始稳定的时间
    bool m_inStableState;           // 是否处于稳定状态
};

// ------------------- 全功能阶段：Edge Impulse 识别器 -------------------
// 将在模型训练完成并导出 Arduino 库后新增 EdgeImpulseRecognizer 类与对应实现。
// 届时需要同步确定：窗口长度、采样率、特征通道布局、label→GestureType 映射表，
// 再在此处声明类，并在 createGestureRecognizer() 中接入。当前阶段不预留任何空壳。

// ------------------- 工厂函数 -------------------
// 创建当前阶段适用的识别器实例。当前仅返回 RuleBasedRecognizer。
// 调用者负责 delete 返回的指针。
GestureRecognizer* createGestureRecognizer();

#endif // GESTURE_RECOGNIZER_H
