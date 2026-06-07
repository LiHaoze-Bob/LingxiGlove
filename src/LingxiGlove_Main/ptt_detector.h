// ============================================================
// ptt_detector.h
// PTT (Push-to-Talk) 双击检测器 — 阶段 A
//
// 设计要点：
//   - 阶段 A（首发 MVP）：仅检测两次加速度模长尖峰即触发录音；
//     录音中检测到握拳手势（≥ PTT_FIST_MIN_FINGERS 指弯曲）即结束。
//     不要求手掌张开、不要求陀螺仪静止 — 先实测真实手感与误触发率。
//   - 阶段 B：当阶段 A 误触发率不可接受时启用，叠加 5 指张开 + 身体
//     静止两个前置守卫（由 PTT_PHASE_B_GUARDS 编译宏切换）。
//   - 状态机与前端 MicState 完全对齐（IDLE/WAITING_TAP/ARMED/
//     RECORDING/PROCESSING），方便通过 WebSocket 直接广播。
//   - 不依赖 SensorData，输入参数明确，便于在 host 上做单元测试。
//
// 参考：
//   - config.h 中的 PTT_TAP_DELTA_G / PTT_TAP_GAP_*_MS / PTT_FIST_*
//   - learned_skill_experience：手势识别器持续输出稳定结果技能
// ============================================================
#ifndef PTT_DETECTOR_H
#define PTT_DETECTOR_H

#include <stdint.h>
#include <stddef.h>

#include "config.h"

#if !defined(ENABLE_PTT)
#define ENABLE_PTT 0
#endif

// PTT 状态机：与前端 src/lib/types.ts 中的 MicState 一一对应
enum PttState : uint8_t {
    PTT_STATE_IDLE         = 0,
    PTT_STATE_WAITING_TAP  = 1,  // 等待第 1 击（保留状态以便前端动画）
    PTT_STATE_ARMED        = 2,  // 已检测到第 1 击，等第 2 击
    PTT_STATE_RECORDING    = 3,  // 双击成功，正在录音
    PTT_STATE_PROCESSING   = 4   // 录音结束，等待 ASR 返回
};

// 单帧观测 — 不依赖 SensorData，便于 host 测试与替换硬件
struct PttSample {
    float        accel_x;     // g
    float        accel_y;     // g
    float        accel_z;     // g
    float        gyro_x;      // deg/s
    float        gyro_y;      // deg/s
    float        gyro_z;      // deg/s
    const float* flex_norm;   // 长度 = flex_count，每路 0~1
    uint8_t      flex_count;  // 通常为 FLEX_CHANNEL_COUNT (5)
};

// 单次 Update 的诊断输出
struct PttDecision {
    PttState state;            // 更新后的状态
    bool     state_changed;    // 相比上一次是否翻转
    bool     start_recording;  // 本帧应开始录音（上层用此驱动麦克风启动）
    bool     stop_recording;   // 本帧应停止录音并进入 ASR
    float    accel_delta_g;    // 当前帧 ||a| - 1g|，便于前端 IMU 卡显示
};

class PttDetector {
public:
    PttDetector();

    // 复位状态机到 IDLE，清空击打时间戳
    void Reset();

    // 喂一帧观测，返回本帧决策
    PttDecision Update(const PttSample& sample, uint32_t now_ms);

    // 上层在 ASR 完成（或失败）后必须调用此函数，将 PROCESSING 切回 IDLE
    void NotifyAsrFinished();

    // 当前状态查询（不改变状态）
    PttState GetState() const { return state_; }

    // 当前状态进入时间（ms 时间戳，用于 RECORDING 时长展示）
    uint32_t GetStateStartedMs() const { return state_started_ms_; }

private:
    // 阶段 B 守卫：5 指全张开 + 身体静止；阶段 A 直接返回 true
    bool PassPhaseGuards(const PttSample& sample) const;

    // 握拳判定：≥ PTT_FIST_MIN_FINGERS 路 flex_norm 超阈值
    bool IsFist(const PttSample& sample) const;

    PttState state_;
    uint32_t state_started_ms_;
    uint32_t last_peak_ms_;   // 最近一次 |a-1g| 超阈的时间戳（含不应期）
    uint32_t last_tap_ms_;    // 最近一次被记账为"有效击打"的时间戳
};

#endif  // PTT_DETECTOR_H
