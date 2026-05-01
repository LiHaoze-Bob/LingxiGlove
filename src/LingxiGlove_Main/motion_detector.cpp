// ============================================================
// motion_detector.cpp
// 见 motion_detector.h 头部的设计说明
// ============================================================

#include "motion_detector.h"

#include <math.h>

// 编译期一致性检查：防止有人把 MOTION_WIN_SIZE 改成超过内部静态数组容量
#if MOTION_WIN_SIZE > MOTION_WIN_MAX
#error "MOTION_WIN_SIZE must be <= MOTION_WIN_MAX; adjust config.h"
#endif
#if MOTION_WIN_SIZE < 2
#error "MOTION_WIN_SIZE must be >= 2 to compute variance"
#endif

MotionDetector::MotionDetector() {
    Reset();
}

void MotionDetector::Reset() {
    for (size_t i = 0; i < MOTION_WIN_MAX; i++) {
        accel_mag_buf_[i] = 0.0f;
    }
    buf_head_   = 0;
    buf_count_  = 0;
    state_      = MOTION_STATE_STILL;
    still_hold_ = 0;
}

void MotionDetector::PushAccelMagnitude(float mag) {
    accel_mag_buf_[buf_head_] = mag;
    buf_head_ = (buf_head_ + 1) % (size_t)MOTION_WIN_SIZE;
    if (buf_count_ < (size_t)MOTION_WIN_SIZE) {
        buf_count_++;
    }
}

void MotionDetector::ComputeAccelStats(float* out_mean, float* out_variance) const {
    if (buf_count_ == 0) {
        *out_mean     = 0.0f;
        *out_variance = 0.0f;
        return;
    }
    // 两遍算法：先均值，再方差。虽然单遍 Welford 更省一次遍历，
    // 但窗口定长 <= 32，常数差异可以忽略；双遍实现可读性更好。
    float sum = 0.0f;
    for (size_t i = 0; i < buf_count_; i++) {
        sum += accel_mag_buf_[i];
    }
    const float mean = sum / (float)buf_count_;

    float sq_err_sum = 0.0f;
    for (size_t i = 0; i < buf_count_; i++) {
        const float d = accel_mag_buf_[i] - mean;
        sq_err_sum += d * d;
    }
    // 使用总体方差（除以 N 而不是 N-1）：window 被视为完整观测样本，
    // 阈值也是按此口径标定的。
    *out_mean     = mean;
    *out_variance = sq_err_sum / (float)buf_count_;
}

MotionDecision MotionDetector::Update(const MotionSample& sample) {
    // 1. 计算本帧的 |a| 与 |gyro|
    const float accel_mag = sqrtf(
        sample.accel_x * sample.accel_x +
        sample.accel_y * sample.accel_y +
        sample.accel_z * sample.accel_z);

    const float gyro_mag = sqrtf(
        sample.gyro_x * sample.gyro_x +
        sample.gyro_y * sample.gyro_y +
        sample.gyro_z * sample.gyro_z);

    // 2. 入环形窗口
    PushAccelMagnitude(accel_mag);

    // 3. 窗口统计
    float mean = 0.0f, variance = 0.0f;
    ComputeAccelStats(&mean, &variance);

    // 4. 状态机（双阈值滞回 + 静止保持计数）
    const MotionState prev_state = state_;

    if (state_ == MOTION_STATE_STILL) {
        // 从 STILL 切到 MOVING：任一特征过 ENTER 阈值即可（更敏感，避免漏触发）
        if (variance > (float)MOTION_VAR_ENTER ||
            gyro_mag > (float)MOTION_GYRO_ENTER) {
            state_      = MOTION_STATE_MOVING;
            still_hold_ = 0;
        }
    } else { // MOVING
        // 回 STILL：两特征均落回 EXIT 阈值下，且需要连续保持若干帧
        if (variance < (float)MOTION_VAR_EXIT &&
            gyro_mag < (float)MOTION_GYRO_EXIT) {
            still_hold_++;
            if (still_hold_ >= (uint32_t)MOTION_STILL_HOLD_FRAMES) {
                state_      = MOTION_STATE_STILL;
                still_hold_ = 0;
            }
        } else {
            // 只要有一帧超阈，计数归零
            still_hold_ = 0;
        }
    }

    // 5. 打包诊断结果
    MotionDecision decision;
    decision.state               = state_;
    decision.state_changed       = (state_ != prev_state);
    decision.accel_mag_variance  = variance;
    decision.gyro_magnitude      = gyro_mag;
    return decision;
}
