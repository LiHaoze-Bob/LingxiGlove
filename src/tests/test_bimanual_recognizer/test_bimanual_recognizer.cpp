// ============================================================
// test_bimanual_recognizer.cpp
// BimanualRuleRecognizer 的 host-side 单元测试（C++11，g++/clang++）
// ------------------------------------------------------------
// 编译运行：见同目录 Makefile，或：
//   g++ -std=c++11 -Wall -Wextra -I. -I../../LingxiGlove_Main \
//       ../../LingxiGlove_Main/gesture_recognizer.cpp \
//       test_bimanual_recognizer.cpp -o run_tests
//   ./run_tests
// ------------------------------------------------------------
// 覆盖范围：
//   T1 Slave 帧超时 → NONE 且重置防抖
//   T2 双手都朝上                              → JIAYOU
//   T3 双手都朝下                              → YIQI
//   T4 双手 roll 对称偏转                       → WOAINI
//   T5 左手抬起 + 右手居中                      → BANGZHU
//   T6 BANGZHU 候选首帧不触发（防抖未到）         → NONE
//   T7 双手都朝上但 roll 仍居中                  → JIAYOU（不被新帮助规则误吃）
//   T8 双手 roll 对称 + slave_pitch 也朝上       → WOAINI（roll 优先于帮助）
//   T9 极端 roll 对称（±85°，接近物理极限）       → WOAINI（极限姿态验证）
// ============================================================

#include "../../LingxiGlove_Main/gesture_recognizer.h"

#include <cstdio>
#include <cstring>

// ------------------------------------------------------------
// 测试时间桩：millis() 由本文件实现，读全局 g_test_now_ms。
// ------------------------------------------------------------
static unsigned long g_test_now_ms = 0;

unsigned long millis(void) {
    return g_test_now_ms;
}

// ------------------------------------------------------------
// 极简断言（避免拉 gtest 依赖）
// ------------------------------------------------------------
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

// ------------------------------------------------------------
// 辅助：模拟双手帧持续输入直到防抖触发或失败。
// 调用模式：先调一次让识别器锁定 detected 候选；推进 BIMANUAL_STABLE_MS
// 后再调一次，预期返回最终类型（NONE 表示用例希望不触发）。
// ------------------------------------------------------------
static BimanualGestureResult RunTwoFrames(BimanualRuleRecognizer& rec,
                                          const BimanualInput& input) {
    g_test_now_ms = 0;
    rec.init();
    // 第 1 帧：候选稳定开始时刻 = 0
    BimanualGestureResult r1 = rec.recognize(input);
    (void)r1;
    // 推进到刚好达到防抖阈值
    g_test_now_ms = BIMANUAL_STABLE_MS + 1;
    return rec.recognize(input);
}

// ============================================================
// T1：Slave 帧超时 → NONE 且重置防抖
// ============================================================
static void TestSlaveStale() {
    std::printf("[T1] slave frame stale → NONE\n");
    BimanualRuleRecognizer rec;
    BimanualInput in;
    in.master_pitch        = 40.0f;
    in.slave_pitch         = 40.0f;
    in.master_roll         = 0.0f;
    in.slave_roll          = 0.0f;
    in.slave_frame_age_ms  = BIMANUAL_SLAVE_STALE_MS + 50;  // 超时

    g_test_now_ms = 0;
    rec.init();
    BimanualGestureResult r1 = rec.recognize(in);
    g_test_now_ms = BIMANUAL_STABLE_MS + 100;
    BimanualGestureResult r2 = rec.recognize(in);

    EXPECT(r1.type == BIMANUAL_GESTURE_NONE, "T1a: stale frame, no trigger on first call");
    EXPECT(r2.type == BIMANUAL_GESTURE_NONE, "T1b: stale frame, no trigger after stable_ms");
}

// ============================================================
// T2：双手都朝上 → JIAYOU
// ============================================================
static void TestJiayou() {
    std::printf("[T2] both hands pitch up → JIAYOU\n");
    BimanualRuleRecognizer rec;
    BimanualInput in;
    in.master_pitch        = 40.0f;
    in.slave_pitch         = 45.0f;
    in.master_roll         = 5.0f;     // 居中区内
    in.slave_roll          = -3.0f;
    in.slave_frame_age_ms  = 50;

    BimanualGestureResult r = RunTwoFrames(rec, in);
    EXPECT(r.type == BIMANUAL_GESTURE_JIAYOU, "T2: should detect JIAYOU");
    EXPECT(std::strcmp(r.text, "加油") == 0, "T2: text should be 加油");
}

// ============================================================
// T3：双手都朝下 → YIQI
// ============================================================
static void TestYiqi() {
    std::printf("[T3] both hands pitch down → YIQI\n");
    BimanualRuleRecognizer rec;
    BimanualInput in;
    in.master_pitch        = -40.0f;
    in.slave_pitch         = -45.0f;
    in.master_roll         = 0.0f;
    in.slave_roll          = 0.0f;
    in.slave_frame_age_ms  = 50;

    BimanualGestureResult r = RunTwoFrames(rec, in);
    EXPECT(r.type == BIMANUAL_GESTURE_YIQI, "T3: should detect YIQI");
}

// ============================================================
// T4：双手 roll 对称偏转 → WOAINI
// ============================================================
static void TestWoaini() {
    std::printf("[T4] symmetric roll → WOAINI\n");
    BimanualRuleRecognizer rec;
    BimanualInput in;
    in.master_pitch        = 0.0f;
    in.slave_pitch         = 0.0f;
    in.master_roll         = 40.0f;
    in.slave_roll          = -40.0f;
    in.slave_frame_age_ms  = 50;

    BimanualGestureResult r = RunTwoFrames(rec, in);
    EXPECT(r.type == BIMANUAL_GESTURE_WOAINI, "T4: should detect WOAINI");
}

// ============================================================
// T5：左手托举 + 右手居中 → BANGZHU
// ============================================================
static void TestBangzhu() {
    std::printf("[T5] slave up + master neutral → BANGZHU\n");
    BimanualRuleRecognizer rec;
    BimanualInput in;
    in.master_pitch        = 5.0f;     // 居中区内 |.| < 20
    in.slave_pitch         = 45.0f;    // > 30
    in.master_roll         = -8.0f;    // 居中区内
    in.slave_roll          = 10.0f;    // 不要求 slave_roll 居中
    in.slave_frame_age_ms  = 50;

    BimanualGestureResult r = RunTwoFrames(rec, in);
    EXPECT(r.type == BIMANUAL_GESTURE_BANGZHU, "T5: should detect BANGZHU");
    EXPECT(std::strcmp(r.text, "帮助") == 0, "T5: text should be 帮助");
    EXPECT(r.confidence > 0.0f && r.confidence <= 1.0f,
           "T5: confidence in (0, 1]");
}

// ============================================================
// T6：BANGZHU 候选首帧不触发（防抖未到）
// ============================================================
static void TestBangzhuDebounce() {
    std::printf("[T6] BANGZHU first frame should not trigger\n");
    BimanualRuleRecognizer rec;
    BimanualInput in;
    in.master_pitch        = 5.0f;
    in.slave_pitch         = 45.0f;
    in.master_roll         = -8.0f;
    in.slave_roll          = 10.0f;
    in.slave_frame_age_ms  = 50;

    g_test_now_ms = 0;
    rec.init();
    BimanualGestureResult r1 = rec.recognize(in);
    EXPECT(r1.type == BIMANUAL_GESTURE_NONE,
           "T6a: first frame should not trigger (debounce not elapsed)");

    // 推进时间但仍小于阈值
    g_test_now_ms = BIMANUAL_STABLE_MS - 10;
    BimanualGestureResult r2 = rec.recognize(in);
    EXPECT(r2.type == BIMANUAL_GESTURE_NONE,
           "T6b: still under stable_ms threshold");
}

// ============================================================
// T7：双手都朝上但 roll 居中 → JIAYOU（帮助规则要求 master_pitch 居中）
// ------------------------------------------------------------
// 这是优先级关键用例：旧规则下应识别为 JIAYOU，新增帮助规则不能把
// 它误吃掉（因为 master_pitch=40 不在 ±20° 中性区）
// ============================================================
static void TestJiayouNotEatenByBangzhu() {
    std::printf("[T7] master_pitch high → JIAYOU, NOT BANGZHU\n");
    BimanualRuleRecognizer rec;
    BimanualInput in;
    in.master_pitch        = 40.0f;    // > 帮助中性区 20°
    in.slave_pitch         = 40.0f;
    in.master_roll         = 5.0f;
    in.slave_roll          = 5.0f;
    in.slave_frame_age_ms  = 50;

    BimanualGestureResult r = RunTwoFrames(rec, in);
    EXPECT(r.type == BIMANUAL_GESTURE_JIAYOU,
           "T7: master_pitch=40 disqualifies BANGZHU, should be JIAYOU");
}

// ============================================================
// T8：roll 对称 + slave_pitch 也朝上 → WOAINI（roll 优先于帮助）
// ------------------------------------------------------------
// 优先级关键用例：master_roll=+40 && slave_roll=-40 满足 WOAINI；同时
// slave_pitch=+40 + master_pitch≈0 + master_roll≈±20 也可能命中帮助。
// 测试规则按代码里的优先级——WOAINI 的 if 在前，应该先匹配。
// （此处构造 master_roll=40 已超出帮助的 ±20° 中性区，所以本来就不会
// 命中帮助；这个测试主要保证 WOAINI 仍然正确触发，不被破坏。）
// ============================================================
static void TestWoainiPriority() {
    std::printf("[T8] symmetric roll + slave pitch up → WOAINI\n");
    BimanualRuleRecognizer rec;
    BimanualInput in;
    in.master_pitch        = 5.0f;
    in.slave_pitch         = 40.0f;    // 帮助单独看会成立，但 master_roll 越界
    in.master_roll         = 40.0f;
    in.slave_roll          = -40.0f;
    in.slave_frame_age_ms  = 50;

    BimanualGestureResult r = RunTwoFrames(rec, in);
    EXPECT(r.type == BIMANUAL_GESTURE_WOAINI,
           "T8: roll-symmetric should win over BANGZHU");
}

// ============================================================
// T9：极端 roll 对称（master≈+85°, slave≈-85°）→ WOAINI
// ------------------------------------------------------------
// 场景：右手手掌向左倾至极限（roll≈+85°），左手手掌向右倾至极限
// （roll≈-85°），模拟双手交叉的"这是一个双手测试的指令"。
// roll 物理范围为 ±90°，取 ±85° 代表接近极限但不触碰传感器噪声区。
// 属于 WOAINI 的极限姿态验证，确保识别器在 roll 达到物理极值时
// 仍能正确触发。
// ============================================================
static void TestExtremeRollSymmetry() {
    std::printf("[T9] extreme roll symmetry (±85°, palm-up/palm-down) → WOAINI\n");
    BimanualRuleRecognizer rec;
    BimanualInput in;
    in.master_pitch        = 0.0f;      // pitch 居中，不触发加油/一起
    in.slave_pitch         = 0.0f;
    in.master_roll         = 85.0f;     // 右手向左倾至极限（接近 +90°）
    in.slave_roll          = -85.0f;    // 左手向右倾至极限（接近 -90°）
    in.slave_frame_age_ms  = 50;

    BimanualGestureResult r = RunTwoFrames(rec, in);
    EXPECT(r.type == BIMANUAL_GESTURE_WOAINI,
           "T9: extreme symmetric roll should detect WOAINI");
    EXPECT(std::strcmp(r.text, "我爱你") == 0,
           "T9: text should be 我爱你");
    // WOAINI 置信度基于 avg_roll / 180°，±85° 时约 0.47
    EXPECT(r.confidence > 0.0f && r.confidence <= 1.0f,
           "T9: confidence in (0, 1]");
}

int main() {
    std::printf("========================================\n");
    std::printf(" BimanualRuleRecognizer host-side tests\n");
    std::printf(" PITCH_TH=%.1f  PITCH_DOWN_TH=%.1f\n",
                (double)BIMANUAL_PITCH_THRESHOLD_DEG,
                (double)BIMANUAL_PITCH_DOWN_THRESHOLD_DEG);
    std::printf(" ROLL_TH=%.1f   STABLE_MS=%d\n",
                (double)BIMANUAL_ROLL_THRESHOLD_DEG,
                (int)BIMANUAL_STABLE_MS);
    std::printf(" HELP_SLAVE_PITCH=%.1f  HELP_MASTER_NEUTRAL=%.1f\n",
                (double)BIMANUAL_HELP_SLAVE_PITCH_DEG,
                (double)BIMANUAL_HELP_MASTER_NEUTRAL_DEG);
    std::printf("========================================\n");

    TestSlaveStale();
    TestJiayou();
    TestYiqi();
    TestWoaini();
    TestBangzhu();
    TestBangzhuDebounce();
    TestJiayouNotEatenByBangzhu();
    TestWoainiPriority();
    TestExtremeRollSymmetry();

    std::printf("----------------------------------------\n");
    std::printf(" PASSED: %d   FAILED: %d\n", g_pass, g_fail);
    std::printf("----------------------------------------\n");
    return g_fail == 0 ? 0 : 1;
}
