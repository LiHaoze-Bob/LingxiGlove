// ============================================================
// gesture_recognizer.cpp
// 手势识别引擎实现
// ============================================================

#include "gesture_recognizer.h"
#include "config.h"

// ------------------- 手势文本映射表 -------------------
static const char* s_gestureTexts[GESTURE_COUNT] = {
    "",         // GESTURE_NONE
    "你好",     // GESTURE_HELLO
    "谢谢",     // GESTURE_THANKS
    "再见",     // GESTURE_GOODBYE
    "是",       // GESTURE_YES
    "不"        // GESTURE_NO
};

// ============================================================
// RuleBasedRecognizer 实现
// ============================================================

bool RuleBasedRecognizer::init() {
    m_lastDetected = GESTURE_NONE;
    m_stableStartMs = 0;
    m_inStableState = false;
    DEBUG_PRINTLN("[Gesture] 规则识别器初始化完成");
    DEBUG_PRINTLN("[Gesture] 手势映射: 朝上=你好, 朝下=谢谢, 左倾=再见, 右倾=是, 竖直=不");
    return true;
}

GestureResult RuleBasedRecognizer::recognize(const SensorData& data) {
    GestureResult result = { GESTURE_NONE, "", 0.0f };

    if (!data.mpuValid) {
        return result;
    }

    float pitch = data.pitch;
    float roll  = data.roll;

    // 根据姿态角判定手势
    GestureType detected = GESTURE_NONE;

    if (pitch > ANGLE_THRESHOLD) {
        // 手掌朝上
        detected = GESTURE_HELLO;
    } else if (pitch < -ANGLE_THRESHOLD) {
        // 手掌朝下
        detected = GESTURE_THANKS;
    } else if (roll > ANGLE_THRESHOLD) {
        // 手掌左倾（取决于传感器安装方向，可能需要调整符号）
        detected = GESTURE_GOODBYE;
    } else if (roll < -ANGLE_THRESHOLD) {
        // 手掌右倾
        detected = GESTURE_YES;
    } else if (fabs(pitch) < NEUTRAL_ZONE && fabs(roll) < NEUTRAL_ZONE) {
        // 竖直/水平中性姿态（握拳或平放）
        detected = GESTURE_NO;
    }

    // ------------------- 防抖逻辑 -------------------
    // 同一手势需持续稳定 GESTURE_STABLE_MS 才确认
    if (detected == m_lastDetected && detected != GESTURE_NONE) {
        if (!m_inStableState) {
            if (millis() - m_stableStartMs >= GESTURE_STABLE_MS) {
                m_inStableState = true;
                // 稳定时间达到阈值，确认手势
                result.type = detected;
                result.text = s_gestureTexts[detected];
                // 置信度与角度偏离程度成正比（越偏离中心越确信）
                if (detected == GESTURE_HELLO || detected == GESTURE_THANKS) {
                    result.confidence = min(fabs(pitch) / 90.0f, 1.0f);
                } else if (detected == GESTURE_GOODBYE || detected == GESTURE_YES) {
                    result.confidence = min(fabs(roll) / 90.0f, 1.0f);
                } else {
                    result.confidence = 0.7f;
                }
            }
        }
    } else {
        // 手势变化，重置稳定计时
        m_lastDetected = detected;
        m_stableStartMs = millis();
        m_inStableState = false;
    }

    return result;
}

// ============================================================
// 工厂函数
// ============================================================

GestureRecognizer* createGestureRecognizer() {
    return new RuleBasedRecognizer();
}
