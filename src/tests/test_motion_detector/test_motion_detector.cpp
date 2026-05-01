// ============================================================
// test_motion_detector.cpp
// MotionDetector 的 host-side 单元测试（纯 C++11，g++/clang++ 均可编译）
// ------------------------------------------------------------
// 编译运行：
//   cd src/tests/test_motion_detector
//   g++ -std=c++11 -Wall -Wextra -I. -I../../LingxiGlove_Main \
//       ../../LingxiGlove_Main/motion_detector.cpp \
//       test_motion_detector.cpp -o run_tests
//   ./run_tests
// ------------------------------------------------------------
// 本测试覆盖 motion_detector.h 描述的所有关键行为：
//   T1 静止样本保持 STILL
//   T2 大运动样本一帧内切 MOVING
//   T3 滞回防抖（介于 ENTER/EXIT 之间不翻转）
//   T4 MOVING → STILL 需要 MOTION_STILL_HOLD_FRAMES 连续保持
//   T5 Reset 清零状态
//   T6 窗口未填满时方差计算不 NaN，且不误触发
// ============================================================

// 本测试通过自带的 test_config_stub.h 提供 MOTION_* 宏，
// 覆盖 LingxiGlove_Main/config.h 里的 Arduino 特定内容，
// 让 motion_detector.cpp 在 PC 上可编译。
//
// 技巧：通过 -I. 把本目录放在 include 路径首位，
// motion_detector.h 里的 #include "config.h" 会先命中本目录下
// 的 config.h（即 test_config_stub.h 的软链副本）。
//
// 为了不污染仓库，这里直接在 test cpp 文件最顶部用 #include 的
// 方式让编译器读本目录下的 config.h 文件，见 CMake/g++ 命令注释。
//
// 实际做法：本目录下放一个 config.h（下面的 create_file 会创建），
// g++ 命令中 -I. 位于 -I../../LingxiGlove_Main 之前，即会优先被找到。

#include "../../LingxiGlove_Main/motion_detector.h"

#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <cstring>

// 简易 assert 框架（避免拉入 gtest 依赖）
static int g_pass = 0;
static int g_fail = 0;

#define EXPECT(cond, msg) do {                                  \
    if (cond) {                                                 \
        g_pass++;                                               \
    } else {                                                    \
        g_fail++;                                               \
        std::fprintf(stderr, "[FAIL] %s:%d  %s  (cond: %s)\n",  \
                     __FILE__, __LINE__, msg, #cond);           \
    }                                                           \
} while (0)

static MotionSample MakeStillSample(float noise_scale) {
    // 静止样本：重力朝 Z，小量白噪声（±noise_scale g）
    MotionSample s;
    // 这里用 deterministic 伪随机，避免依赖 <random> 的跨平台差异。
    // 映射到 [-1, 1]: (seed_high_24bit / 0xFFFFFF) * 2 - 1
    static uint32_t seed = 0x12345678u;
    auto rand_unit = [&]() -> float {
        seed = seed * 1664525u + 1013904223u;
        const uint32_t top24 = seed >> 8;             // 0..0xFFFFFF
        return ((float)top24 / (float)0xFFFFFFu) * 2.0f - 1.0f;
    };
    s.accel_x = rand_unit() * noise_scale;
    s.accel_y = rand_unit() * noise_scale;
    s.accel_z = 1.0f + rand_unit() * noise_scale;
    s.gyro_x  = rand_unit() * 0.5f;
    s.gyro_y  = rand_unit() * 0.5f;
    s.gyro_z  = rand_unit() * 0.5f;
    return s;
}

static MotionSample MakeMovingSample(float accel_span, float gyro_span) {
    // 运动样本：大幅摆动
    MotionSample s;
    static int phase = 0;
    phase = (phase + 1) % 4;
    const float sign = (phase % 2 == 0) ? 1.0f : -1.0f;
    s.accel_x = sign * accel_span;
    s.accel_y = -sign * accel_span;
    s.accel_z = 1.0f + sign * accel_span * 0.5f;
    s.gyro_x  = sign * gyro_span;
    s.gyro_y  = -sign * gyro_span;
    s.gyro_z  = sign * gyro_span * 0.5f;
    return s;
}

// ------------------------------------------------------------
// T1：静止样本持续喂入，状态应保持 STILL
// ------------------------------------------------------------
static void TestStillStaysStill() {
    std::printf("[T1] static noise keeps STILL\n");
    MotionDetector det;
    det.Reset();
    int transitions = 0;
    for (int i = 0; i < 60; i++) {
        MotionDecision d = det.Update(MakeStillSample(0.01f /* ±0.01 g */));
        if (d.state_changed) transitions++;
    }
    EXPECT(det.GetState() == MOTION_STATE_STILL, "T1: final state should be STILL");
    EXPECT(transitions == 0, "T1: no state changes expected under quiet input");
}

// ------------------------------------------------------------
// T2：切换到大运动样本，应很快 (≤ MOTION_WIN_SIZE 帧) 翻转 MOVING
// ------------------------------------------------------------
static void TestMovingTriggers() {
    std::printf("[T2] big motion triggers MOVING\n");
    MotionDetector det;
    det.Reset();
    // 先灌静止，把窗口填满
    for (int i = 0; i < MOTION_WIN_SIZE; i++) {
        det.Update(MakeStillSample(0.005f));
    }
    EXPECT(det.GetState() == MOTION_STATE_STILL, "T2: warmup should be STILL");

    // 突然加入大运动，最多 MOTION_WIN_SIZE 帧内必须切 MOVING
    bool moved = false;
    for (int i = 0; i < MOTION_WIN_SIZE; i++) {
        // accel_span=0.5g, gyro=60deg/s，远超 ENTER 阈值
        MotionDecision d = det.Update(MakeMovingSample(0.5f, 60.0f));
        if (d.state == MOTION_STATE_MOVING) {
            moved = true;
            break;
        }
    }
    EXPECT(moved, "T2: MOVING should trigger within MOTION_WIN_SIZE frames");
}

// ------------------------------------------------------------
// T3：滞回防抖
//   在 STILL 状态下喂入"介于 EXIT 和 ENTER 之间"的样本，不应翻转为 MOVING；
//   在 MOVING 状态下喂入同样的中间强度样本，也不应立刻翻回 STILL。
// ------------------------------------------------------------
static void TestHysteresis() {
    std::printf("[T3] hysteresis between ENTER and EXIT keeps state\n");
    MotionDetector det;
    det.Reset();

    // 构造一个特征值介于 EXIT 和 ENTER 之间的陀螺仪幅度：
    //   MOTION_GYRO_EXIT=5.0, MOTION_GYRO_ENTER=15.0 → 取 10.0
    //   MOTION_VAR_EXIT=0.002, MOTION_VAR_ENTER=0.005 → 加速度用 0.04 g 幅度
    //     (var(|a|) ≈ 0.003 g² 大致位于区间中部)
    // 先确认 STILL 下，区间样本不会切 MOVING
    for (int i = 0; i < 40; i++) {
        MotionDecision d = det.Update(MakeMovingSample(0.04f, 10.0f));
        (void)d;
    }
    EXPECT(det.GetState() == MOTION_STATE_STILL,
           "T3a: mid-band input from STILL should not cross ENTER");

    // 再先强制切到 MOVING，然后用区间样本，不应立刻回 STILL
    det.Reset();
    for (int i = 0; i < MOTION_WIN_SIZE; i++) {
        det.Update(MakeMovingSample(0.5f, 60.0f));  // 强运动触发 MOVING
    }
    EXPECT(det.GetState() == MOTION_STATE_MOVING, "T3b: setup MOVING");

    // 喂入远多于 STILL_HOLD_FRAMES 帧的"中间"样本，不应回 STILL
    for (int i = 0; i < MOTION_STILL_HOLD_FRAMES * 5; i++) {
        det.Update(MakeMovingSample(0.04f, 10.0f));
    }
    EXPECT(det.GetState() == MOTION_STATE_MOVING,
           "T3c: mid-band input from MOVING should not fall below EXIT");
}

// ------------------------------------------------------------
// T4：MOVING → STILL 需要 MOTION_STILL_HOLD_FRAMES 帧连续低于 EXIT
// ------------------------------------------------------------
static void TestStillHold() {
    std::printf("[T4] MOVING→STILL requires HOLD frames\n");
    MotionDetector det;
    det.Reset();
    // 先进入 MOVING
    for (int i = 0; i < MOTION_WIN_SIZE; i++) {
        det.Update(MakeMovingSample(0.5f, 60.0f));
    }
    EXPECT(det.GetState() == MOTION_STATE_MOVING, "T4: setup MOVING");

    // 喂入恰好 MOTION_STILL_HOLD_FRAMES - 1 帧的安静样本，状态仍应为 MOVING
    for (int i = 0; i < MOTION_STILL_HOLD_FRAMES - 1; i++) {
        MotionDecision d = det.Update(MakeStillSample(0.002f));
        (void)d;
    }
    EXPECT(det.GetState() == MOTION_STATE_MOVING,
           "T4a: not enough quiet frames yet, stay MOVING");

    // 窗口里仍残留几个高能量样本，所以方差可能没立刻掉到 EXIT 以下；
    // 继续喂入足够多帧直到窗口被新安静样本覆盖满。
    // 只要在"窗口内全是安静样本"之后再累计 MOTION_STILL_HOLD_FRAMES 帧，
    // 就必须进入 STILL。
    bool back_to_still = false;
    for (int i = 0; i < MOTION_WIN_SIZE + MOTION_STILL_HOLD_FRAMES + 10; i++) {
        MotionDecision d = det.Update(MakeStillSample(0.002f));
        if (d.state == MOTION_STATE_STILL) {
            back_to_still = true;
            break;
        }
    }
    EXPECT(back_to_still, "T4b: eventually return to STILL after quiet window");
}

// ------------------------------------------------------------
// T5：Reset 把状态清零
// ------------------------------------------------------------
static void TestReset() {
    std::printf("[T5] Reset clears state\n");
    MotionDetector det;
    for (int i = 0; i < MOTION_WIN_SIZE; i++) {
        det.Update(MakeMovingSample(0.5f, 60.0f));
    }
    EXPECT(det.GetState() == MOTION_STATE_MOVING, "T5a: pre-reset MOVING");
    det.Reset();
    EXPECT(det.GetState() == MOTION_STATE_STILL, "T5b: post-reset STILL");

    // Reset 后喂几个低强度帧，状态保持 STILL
    for (int i = 0; i < 5; i++) {
        det.Update(MakeStillSample(0.002f));
    }
    EXPECT(det.GetState() == MOTION_STATE_STILL, "T5c: stays STILL after reset+quiet");
}

// ------------------------------------------------------------
// T6：窗口未填满时行为稳健（方差不 NaN、不误触发）
// ------------------------------------------------------------
static void TestWarmup() {
    std::printf("[T6] warmup (window not full) is robust\n");
    MotionDetector det;
    det.Reset();
    // 只喂 1 帧
    MotionDecision d1 = det.Update(MakeStillSample(0.005f));
    EXPECT(!std::isnan(d1.accel_mag_variance), "T6a: variance not NaN on 1 sample");
    EXPECT(d1.state == MOTION_STATE_STILL, "T6b: warmup stays STILL on quiet");
    // 只喂 2 帧
    MotionDecision d2 = det.Update(MakeStillSample(0.005f));
    EXPECT(!std::isnan(d2.accel_mag_variance), "T6c: variance not NaN on 2 samples");
}

int main() {
    std::printf("========================================\n");
    std::printf(" MotionDetector host-side unit tests\n");
    std::printf(" MOTION_WIN_SIZE=%d  STILL_HOLD=%d\n",
                (int)MOTION_WIN_SIZE, (int)MOTION_STILL_HOLD_FRAMES);
    std::printf(" VAR  ENTER=%.4f  EXIT=%.4f\n",
                (double)MOTION_VAR_ENTER, (double)MOTION_VAR_EXIT);
    std::printf(" GYRO ENTER=%.2f    EXIT=%.2f\n",
                (double)MOTION_GYRO_ENTER, (double)MOTION_GYRO_EXIT);
    std::printf("========================================\n");

    TestStillStaysStill();
    TestMovingTriggers();
    TestHysteresis();
    TestStillHold();
    TestReset();
    TestWarmup();

    std::printf("----------------------------------------\n");
    std::printf(" PASSED: %d   FAILED: %d\n", g_pass, g_fail);
    std::printf("----------------------------------------\n");
    return g_fail == 0 ? 0 : 1;
}
