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

// ============================================================
// BimanualRuleRecognizer 实现
// ============================================================

static const char* s_bimanualGestureTexts[] = {
    "",     // BIMANUAL_GESTURE_NONE
    "加油", // BIMANUAL_GESTURE_JIAYOU
};

BimanualRuleRecognizer::BimanualRuleRecognizer()
    : m_last_detected_(BIMANUAL_GESTURE_NONE)
    , m_stable_start_ms_(0)
    , m_in_stable_state_(false) {
}

void BimanualRuleRecognizer::init() {
    m_last_detected_   = BIMANUAL_GESTURE_NONE;
    m_stable_start_ms_ = 0;
    m_in_stable_state_ = false;
    DEBUG_PRINTLN("[BimanualGesture] 双手规则识别器初始化完成");
    DEBUG_PRINTLN("[BimanualGesture] 手势: 双手 pitch > 30° → 加油");
}

BimanualGestureResult BimanualRuleRecognizer::recognize(const BimanualInput& input) {
    BimanualGestureResult result = { BIMANUAL_GESTURE_NONE, "", 0.0f };

    // Slave 帧超时检查：帧过期不做双手判断，重置防抖
    if (input.slave_frame_age_ms > BIMANUAL_SLAVE_STALE_MS) {
        if (m_last_detected_ != BIMANUAL_GESTURE_NONE) {
            DEBUG_LOG("[BimanualGesture] Slave 帧超时 (%lums)，重置防抖",
                      (unsigned long)input.slave_frame_age_ms);
        }
        m_last_detected_   = BIMANUAL_GESTURE_NONE;
        m_stable_start_ms_ = 0;
        m_in_stable_state_ = false;
        return result;
    }

    // 规则判断：双手 pitch 同时超过阈值 → "加油"候选
    BimanualGestureType detected = BIMANUAL_GESTURE_NONE;
    if (input.master_pitch > BIMANUAL_PITCH_THRESHOLD_DEG &&
        input.slave_pitch  > BIMANUAL_PITCH_THRESHOLD_DEG) {
        detected = BIMANUAL_GESTURE_JIAYOU;
    }

    // 防抖：候选手势需持续稳定 BIMANUAL_STABLE_MS 才确认
    unsigned long now = millis();
    if (detected == m_last_detected_ && detected != BIMANUAL_GESTURE_NONE) {
        if (!m_in_stable_state_) {
            if (now - m_stable_start_ms_ >= BIMANUAL_STABLE_MS) {
                m_in_stable_state_ = true;
                // 确认手势：置信度取双手 pitch 均值与阈值之比，上限 1.0
                float avg_pitch = (input.master_pitch + input.slave_pitch) * 0.5f;
                result.type       = detected;
                result.text       = s_bimanualGestureTexts[detected];
                result.confidence = (avg_pitch / 90.0f < 1.0f) ? avg_pitch / 90.0f : 1.0f;
                DEBUG_LOG("[BimanualGesture] 确认手势: %s  master_pitch=%.1f  slave_pitch=%.1f  age=%lums",
                          result.text,
                          (double)input.master_pitch,
                          (double)input.slave_pitch,
                          (unsigned long)input.slave_frame_age_ms);
            }
        }
        // 已处于稳定状态：本帧仍持续触发则不重复返回（由调用方的冷却时间管控）
    } else {
        // 候选手势变化（或从 NONE 变为有效），重置防抖计时
        if (detected != m_last_detected_) {
            m_last_detected_   = detected;
            m_stable_start_ms_ = now;
            m_in_stable_state_ = false;
        }
    }

    return result;
}
