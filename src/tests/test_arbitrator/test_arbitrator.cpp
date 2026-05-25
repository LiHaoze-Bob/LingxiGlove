// ============================================================
// test_arbitrator.cpp
// GestureArbitrator 的 host-side 单元测试（C++11，g++/clang++）
// ------------------------------------------------------------
// 编译运行：见同目录 Makefile，或：
//   g++ -std=c++11 -Wall -Wextra -I. -I../../LingxiGlove_Main \
//       -include config.h \
//       ../../LingxiGlove_Main/gesture_arbitrator.cpp \
//       test_arbitrator.cpp -o run_tests
//   ./run_tests
// ------------------------------------------------------------
// 覆盖范围：
//   T1  仅单手候选 → 确认后播报
//   T2  仅双手候选 → 确认后播报
//   T3  单手+双手同时 → 双手胜出，单手被抑制
//   T4  冷却期内 → 不播报
//   T5  去重：相同手势不重复播报
//   T6  确认窗口：候选未持续够时间 → 不播报
//   T7  手势切换：新手势可再次播报
//   T8  双手消失后 → 单手可以播报（经确认窗口）
// ============================================================

#include "../../LingxiGlove_Main/gesture_arbitrator.h"

#include <cstdio>
#include <cstring>

// ------------------------------------------------------------
// 极简断言
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
// 辅助：构造候选
// ------------------------------------------------------------
static GestureCandidate MakeSingle(const char* text, float conf) {
    return { GESTURE_SOURCE_SINGLE_HAND, text, conf };
}

static GestureCandidate MakeBimanual(const char* text, float conf) {
    return { GESTURE_SOURCE_BIMANUAL, text, conf };
}

static GestureCandidate MakeNone() {
    return { GESTURE_SOURCE_NONE, "", 0.0f };
}

// 辅助：持续 tick N 帧（每帧 50ms），返回最后一帧结果
static ArbitratedGesture TickN(GestureArbitrator& arb,
                               const GestureCandidate& single,
                               const GestureCandidate& bimanual,
                               int frames, unsigned long& now_ms) {
    ArbitratedGesture r = { false, GESTURE_SOURCE_NONE, "", 0.0f };
    for (int i = 0; i < frames; i++) {
        r = arb.tick(single, bimanual, now_ms);
        now_ms += 50;
    }
    return r;
}

// ============================================================
// 测试用例
// ============================================================

// T1: 仅单手候选 → 确认后播报
static void TestSingleOnly() {
    std::printf("[T1] single-hand only → announce after confirm\n");
    GestureArbitrator arb;
    arb.init();
    unsigned long now = 0;

    // 持续 5 帧 (250ms) > ARBITRATOR_CONFIRM_MS (200ms)
    ArbitratedGesture r = TickN(arb, MakeSingle("你好", 0.8f), MakeNone(), 5, now);
    EXPECT(r.should_announce == true, "T1: should announce after 250ms");
    EXPECT(r.source == GESTURE_SOURCE_SINGLE_HAND, "T1: source should be single");
    EXPECT(std::strcmp(r.text, "你好") == 0, "T1: text should be 你好");
}

// T2: 仅双手候选 → 确认后播报
static void TestBimanualOnly() {
    std::printf("[T2] bimanual only → announce after confirm\n");
    GestureArbitrator arb;
    arb.init();
    unsigned long now = 0;

    ArbitratedGesture r = TickN(arb, MakeNone(), MakeBimanual("加油", 0.7f), 5, now);
    EXPECT(r.should_announce == true, "T2: should announce after 250ms");
    EXPECT(r.source == GESTURE_SOURCE_BIMANUAL, "T2: source should be bimanual");
    EXPECT(std::strcmp(r.text, "加油") == 0, "T2: text should be 加油");
}

// T3: 单手+双手同时 → 双手胜出
static void TestBimanualSuppressesSingle() {
    std::printf("[T3] both present → bimanual wins, single suppressed\n");
    GestureArbitrator arb;
    arb.init();
    unsigned long now = 0;

    // 单手"不" + 双手"帮助" → 双手应胜出
    ArbitratedGesture r = TickN(arb,
        MakeSingle("不", 0.9f),
        MakeBimanual("帮助", 0.6f),
        5, now);
    EXPECT(r.should_announce == true, "T3: should announce");
    EXPECT(r.source == GESTURE_SOURCE_BIMANUAL, "T3: bimanual should win");
    EXPECT(std::strcmp(r.text, "帮助") == 0, "T3: text should be 帮助");
}

// T4: 冷却期内不播报
static void TestCooldown() {
    std::printf("[T4] cooldown prevents re-announce within 2s\n");
    GestureArbitrator arb;
    arb.init();
    unsigned long now = 0;

    // 第一次播报
    ArbitratedGesture r1 = TickN(arb, MakeSingle("你好", 0.8f), MakeNone(), 5, now);
    EXPECT(r1.should_announce == true, "T4: first announce should succeed");

    // 冷却中提交不同手势 → 不播报
    ArbitratedGesture r2 = TickN(arb, MakeSingle("谢谢", 0.8f), MakeNone(), 5, now);
    EXPECT(r2.should_announce == false, "T4: should NOT announce during cooldown");

    // 跳到冷却结束后 → 可以播报
    now += 2000;
    ArbitratedGesture r3 = TickN(arb, MakeSingle("谢谢", 0.8f), MakeNone(), 5, now);
    EXPECT(r3.should_announce == true, "T4: should announce after cooldown");
    EXPECT(std::strcmp(r3.text, "谢谢") == 0, "T4: text should be 谢谢");
}

// T5: 去重 - 相同手势不重复播报
static void TestDedup() {
    std::printf("[T5] same gesture not re-announced\n");
    GestureArbitrator arb;
    arb.init();
    unsigned long now = 0;

    // 第一次播报"你好"
    ArbitratedGesture r1 = TickN(arb, MakeSingle("你好", 0.8f), MakeNone(), 5, now);
    EXPECT(r1.should_announce == true, "T5: first announce should succeed");

    // 冷却结束后，再次提交"你好" → 不播报（去重）
    now += 2000;
    ArbitratedGesture r2 = TickN(arb, MakeSingle("你好", 0.8f), MakeNone(), 5, now);
    EXPECT(r2.should_announce == false, "T5: same gesture should be deduped");
}

// T6: 确认窗口 - 候选未持续够时间
static void TestConfirmWindow() {
    std::printf("[T6] candidate not persisted long enough → no announce\n");
    GestureArbitrator arb;
    arb.init();
    unsigned long now = 0;

    // 只 tick 3 帧 (150ms) < ARBITRATOR_CONFIRM_MS (200ms)
    ArbitratedGesture r = TickN(arb, MakeSingle("你好", 0.8f), MakeNone(), 3, now);
    EXPECT(r.should_announce == false, "T6: should NOT announce before confirm window");
}

// T7: 手势切换 - 新手势可再次播报
static void TestGestureChange() {
    std::printf("[T7] different gesture → new announce\n");
    GestureArbitrator arb;
    arb.init();
    unsigned long now = 0;

    // 播报"你好"
    ArbitratedGesture r1 = TickN(arb, MakeSingle("你好", 0.8f), MakeNone(), 5, now);
    EXPECT(r1.should_announce == true, "T7: first announce should succeed");

    // 冷却结束后，换成"谢谢" → 可以播报
    now += 2000;
    ArbitratedGesture r2 = TickN(arb, MakeSingle("谢谢", 0.7f), MakeNone(), 5, now);
    EXPECT(r2.should_announce == true, "T7: different gesture should announce");
    EXPECT(std::strcmp(r2.text, "谢谢") == 0, "T7: text should be 谢谢");
}

// T8: 双手消失后 → 单手可以播报
static void TestBimanualGoneThenSingle() {
    std::printf("[T8] bimanual gone → single can announce\n");
    GestureArbitrator arb;
    arb.init();
    unsigned long now = 0;

    // 先双手播报
    ArbitratedGesture r1 = TickN(arb, MakeNone(), MakeBimanual("加油", 0.8f), 5, now);
    EXPECT(r1.should_announce == true, "T8: bimanual should announce first");
    EXPECT(std::strcmp(r1.text, "加油") == 0, "T8: text should be 加油");

    // 冷却中双手消失，只剩单手 → 不播报（冷却）
    ArbitratedGesture r2 = TickN(arb, MakeSingle("你好", 0.8f), MakeNone(), 5, now);
    EXPECT(r2.should_announce == false, "T8: should NOT announce during cooldown");

    // 冷却结束 → 单手可以播报
    now += 2000;
    ArbitratedGesture r3 = TickN(arb, MakeSingle("你好", 0.8f), MakeNone(), 5, now);
    EXPECT(r3.should_announce == true, "T8: single should announce after cooldown");
    EXPECT(std::strcmp(r3.text, "你好") == 0, "T8: text should be 你好");
}

// ============================================================
// 入口
// ============================================================
int main() {
    std::printf("=== GestureArbitrator Unit Tests ===\n\n");

    TestSingleOnly();
    TestBimanualOnly();
    TestBimanualSuppressesSingle();
    TestCooldown();
    TestDedup();
    TestConfirmWindow();
    TestGestureChange();
    TestBimanualGoneThenSingle();

    std::printf("\n=== Results: %d passed, %d failed (total %d) ===\n",
                g_pass, g_fail, g_pass + g_fail);
    return g_fail > 0 ? 1 : 0;
}
