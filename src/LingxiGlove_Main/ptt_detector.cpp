// ============================================================
// ptt_detector.cpp
// 见 ptt_detector.h 头部说明
// ============================================================
#include "ptt_detector.h"

#include <math.h>

PttDetector::PttDetector() {
    Reset();
}

void PttDetector::Reset() {
    state_            = PTT_STATE_IDLE;
    state_started_ms_ = 0;
    last_peak_ms_     = 0;
    last_tap_ms_      = 0;
}

bool PttDetector::PassPhaseGuards(const PttSample& sample) const {
#if PTT_PHASE_B_GUARDS
    // 5 路 Flex 全张开（normalized < 0.3 视为伸直）
    if (sample.flex_norm && sample.flex_count > 0) {
        for (uint8_t i = 0; i < sample.flex_count; i++) {
            if (sample.flex_norm[i] > 0.3f) return false;
        }
    }
    // 陀螺仪静止 |ω| < 50°/s
    const float gyro_mag = sqrtf(sample.gyro_x * sample.gyro_x +
                                 sample.gyro_y * sample.gyro_y +
                                 sample.gyro_z * sample.gyro_z);
    if (gyro_mag > 50.0f) return false;
    return true;
#else
    (void)sample;
    return true;  // 阶段 A：无前置守卫
#endif
}

bool PttDetector::IsFist(const PttSample& sample) const {
    if (!sample.flex_norm || sample.flex_count == 0) return false;
    uint8_t bent = 0;
    for (uint8_t i = 0; i < sample.flex_count; i++) {
        if (sample.flex_norm[i] >= (float)PTT_FIST_FINGER_THRESHOLD) {
            bent++;
        }
    }
    return bent >= (uint8_t)PTT_FIST_MIN_FINGERS;
}

PttDecision PttDetector::Update(const PttSample& sample, uint32_t now_ms) {
    PttDecision out;
    out.state           = state_;
    out.state_changed   = false;
    out.start_recording = false;
    out.stop_recording  = false;

    // 加速度模长偏离 1g 的瞬时偏差
    const float a_mag = sqrtf(sample.accel_x * sample.accel_x +
                              sample.accel_y * sample.accel_y +
                              sample.accel_z * sample.accel_z);
    const float delta = fabsf(a_mag - 1.0f);
    out.accel_delta_g = delta;

#if !ENABLE_PTT
    // PTT 关闭：状态机停留在 IDLE，仅返回 accel_delta 供观察
    return out;
#endif

    const PttState prev_state = state_;

    // ─── 击打检测（含不应期）────────────────────────────────────
    bool peak_detected = false;
    if (delta > (float)PTT_TAP_DELTA_G) {
        if ((now_ms - last_peak_ms_) >= (uint32_t)PTT_TAP_REFRACTORY_MS) {
            peak_detected = true;
            last_peak_ms_ = now_ms;
        }
    }

    switch (state_) {
        case PTT_STATE_IDLE: {
            if (peak_detected && PassPhaseGuards(sample)) {
                // 第 1 击 → 进入 ARMED；先短暂经过 WAITING_TAP 让前端动画起跳
                last_tap_ms_      = now_ms;
                state_            = PTT_STATE_ARMED;
                state_started_ms_ = now_ms;
            }
            break;
        }
        case PTT_STATE_WAITING_TAP: {
            // 当前实现 IDLE 直接转 ARMED，此分支保留为未来"长按预备"扩展
            if (peak_detected) {
                last_tap_ms_      = now_ms;
                state_            = PTT_STATE_ARMED;
                state_started_ms_ = now_ms;
            }
            break;
        }
        case PTT_STATE_ARMED: {
            const uint32_t gap = now_ms - last_tap_ms_;
            if (peak_detected &&
                gap >= (uint32_t)PTT_TAP_GAP_MIN_MS &&
                gap <= (uint32_t)PTT_TAP_GAP_MAX_MS) {
                // 双击成功：进入录音
                state_              = PTT_STATE_RECORDING;
                state_started_ms_   = now_ms;
                out.start_recording = true;
            } else if (gap > (uint32_t)PTT_ARMED_TIMEOUT_MS) {
                // 超时未等到第 2 击 → 回 IDLE
                state_            = PTT_STATE_IDLE;
                state_started_ms_ = now_ms;
            }
            break;
        }
        case PTT_STATE_RECORDING: {
            const bool fist     = IsFist(sample);
            const bool too_long = (now_ms - state_started_ms_) >
                                  (uint32_t)PTT_RECORDING_MAX_MS;
            if (fist || too_long) {
                state_             = PTT_STATE_PROCESSING;
                state_started_ms_  = now_ms;
                out.stop_recording = true;
            }
            break;
        }
        case PTT_STATE_PROCESSING: {
            // 等待上层 NotifyAsrFinished()；此处不主动切换
            break;
        }
    }

    if (state_ != prev_state) {
        out.state_changed = true;
    }
    out.state = state_;
    return out;
}

void PttDetector::NotifyAsrFinished() {
    if (state_ == PTT_STATE_PROCESSING) {
        state_            = PTT_STATE_IDLE;
        state_started_ms_ = 0;
    }
}
