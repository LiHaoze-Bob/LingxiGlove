// ============================================================
// motion_detector.h
// 动作/静止二值门控 (VAD for HAR)
// ------------------------------------------------------------
// 设计目标：
//   1. 手语是"动作流"，静止时刻不应触发手势识别；
//   2. 为未来 "动作分割 + 分类" 两段式推理链留接口；
//   3. 逻辑完全用标准 C++11 实现，不依赖 Arduino / FreeRTOS，
//      便于在 PC 上编写 host-side 单元测试。
//
// 门控策略（双阈值滞回）：
//   - MOVING 进入条件：窗口内 |a| 方差 > MOTION_VAR_ENTER
//                    或 |gyro| 模长 > MOTION_GYRO_ENTER
//   - STILL 恢复条件：窗口内 |a| 方差 < MOTION_VAR_EXIT
//                    且 |gyro| 模长 < MOTION_GYRO_EXIT
//                    且上述条件连续持续 MOTION_STILL_HOLD_FRAMES 帧
//
// 阈值由 config.h 提供，默认值基于 MPU6050 datasheet 噪声水平推算，
// 真实佩戴条件下需用采集模式录制静止/运动段后二次调参。
// ============================================================

#ifndef MOTION_DETECTOR_H
#define MOTION_DETECTOR_H

#include <stdint.h>
#include <stddef.h>

#include "config.h"

// ------------------- 状态枚举 -------------------
enum MotionState {
    MOTION_STATE_STILL  = 0,
    MOTION_STATE_MOVING = 1
};

// ------------------- 单帧观测 -------------------
// 为了让本模块不依赖 SensorData 结构（避免循环包含，并方便 host 测试），
// 只输入计算门控必需的最小特征：加速度三轴（g）与陀螺仪三轴（deg/s）。
struct MotionSample {
    float accel_x;  // g
    float accel_y;  // g
    float accel_z;  // g
    float gyro_x;   // deg/s
    float gyro_y;   // deg/s
    float gyro_z;   // deg/s
};

// ------------------- 单次 update 的诊断输出 -------------------
struct MotionDecision {
    MotionState state;             // 更新后的状态
    bool        state_changed;     // 相比上一次是否翻转（供上层打日志）
    float       accel_mag_variance;// 当前窗口 |a| 方差 (g²)
    float       gyro_magnitude;    // 当前帧 |gyro| (deg/s)
};

/**
 * @brief 动作/静止门控器
 *
 * 使用：
 *   MotionDetector det;
 *   det.reset();
 *   MotionDecision d = det.update(sample);
 *   if (d.state == MOTION_STATE_STILL) { skip_recognize(); }
 *
 * 线程安全：非线程安全，仅供单线程（主循环）使用。
 */
class MotionDetector {
public:
    MotionDetector();

    /**
     * @brief 复位内部缓冲与状态。
     *
     * 会把状态清为 STILL、窗口清空、still_hold 计数归零。
     */
    void Reset();

    /**
     * @brief 喂入一帧观测，返回更新后的状态决策。
     *
     * 若窗口尚未填满（样本数 < MOTION_WIN_SIZE），状态保持 STILL，
     * 方差计算仍按当前已有样本数计算以便日志输出。
     */
    MotionDecision Update(const MotionSample& sample);

    /**
     * @brief 返回当前状态（不改变内部窗口）。
     */
    MotionState GetState() const { return state_; }

private:
    // 内部：向环形缓冲区追加一个 |a| 值
    void PushAccelMagnitude(float mag);

    // 内部：基于当前环形缓冲区已有样本计算均值/方差
    void ComputeAccelStats(float* out_mean, float* out_variance) const;

    // 环形缓冲区（定长，避免堆分配）
    float   accel_mag_buf_[MOTION_WIN_MAX];
    size_t  buf_head_;      // 下一次写入位置（0..MOTION_WIN_MAX-1）
    size_t  buf_count_;     // 已填充的样本数，上限 MOTION_WIN_SIZE

    MotionState state_;
    uint32_t    still_hold_;  // 连续满足 EXIT 条件的帧数，达到阈值才切 STILL
};

#endif  // MOTION_DETECTOR_H
