// ============================================================
// test_local_tts_fallback.cpp
// local_tts_fallback 查表逻辑的 host-side 单元测试
// ------------------------------------------------------------
// 说明：
//   local_tts_fallback.cpp 依赖 Arduino Serial 与 tts_player::PlayPcmInt16（I2S）。
//   这里用与 local_tts_fallback.cpp **逐行等价** 的参考实现 PlayOfflineVoice_Ref
//   + 可注入的 PcmTable 来覆盖查表逻辑：命中 / 未命中 / 空指针 / 空表 / 空 label。
//   真正的 I2S 播放在板上验证。
//
// 编译运行：
//   cd src/tests/test_local_tts_fallback
//   g++ -std=c++11 -Wall -Wextra test_local_tts_fallback.cpp -o run_tests
//   ./run_tests
// ============================================================

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstddef>

static int g_pass = 0;
static int g_fail = 0;

#define EXPECT(cond, msg) do {                                  \
    if (cond) { g_pass++; }                                     \
    else {                                                      \
        g_fail++;                                               \
        std::fprintf(stderr, "[FAIL] %s:%d  %s  (cond: %s)\n",  \
                     __FILE__, __LINE__, msg, #cond);           \
    }                                                           \
} while (0)

// ---------- 契约复刻（与 offline_voice_pcm.h 对齐） ----------
struct OfflinePcmEntry {
    const char*    label;
    const int16_t* data;
    size_t         sample_count;
    uint32_t       sample_rate;
};

// ---------- 参考实现：PlayOfflineVoice_Ref 与 local_tts_fallback.cpp 等价 ----------
// 不同点：
//   - 用可注入的 table/count 取代全局 kOfflinePcmTable
//   - 用一个可注入的 mock PlayPcmInt16 (mock_play) 记录调用，不做真 I2S
struct PlayCall {
    const int16_t* pcm;
    size_t         sample_count;
    uint32_t       sample_rate;
    bool           should_succeed;
};
static PlayCall g_last_play;
static int g_play_invoke_count = 0;
// 下一次 mock 播放应该返回的结果（true = 成功）
static bool g_mock_play_result = true;

static bool mock_play(const int16_t* pcm, size_t n, uint32_t sr) {
    g_play_invoke_count++;
    g_last_play.pcm = pcm;
    g_last_play.sample_count = n;
    g_last_play.sample_rate = sr;
    g_last_play.should_succeed = g_mock_play_result;
    return g_mock_play_result;
}

static bool PlayOfflineVoice_Ref(const char* label,
                                 const OfflinePcmEntry* table,
                                 size_t count) {
    if (!label || label[0] == '\0') return false;
    if (count == 0) return false;
    for (size_t i = 0; i < count; i++) {
        const OfflinePcmEntry& e = table[i];
        if (!e.label || !e.data) continue;
        if (std::strcmp(e.label, label) != 0) continue;
        if (!mock_play(e.data, e.sample_count, e.sample_rate)) return false;
        return true;
    }
    return false;
}

static void reset_mock() {
    g_play_invoke_count = 0;
    g_mock_play_result = true;
    g_last_play.pcm = nullptr;
    g_last_play.sample_count = 0;
    g_last_play.sample_rate = 0;
}

// ---------- 固定的 PCM 数据 ----------
static const int16_t kPcmA[] = { 10, 20, 30, 40 };
static const int16_t kPcmB[] = { -1, -2, -3 };

static void TestEmptyTable() {
    std::printf("[T1] empty table returns false, never invokes play\n");
    reset_mock();
    bool r = PlayOfflineVoice_Ref("你好", nullptr, 0);
    EXPECT(r == false, "T1: empty-table -> false");
    EXPECT(g_play_invoke_count == 0, "T1: no play() invoked");
}

static void TestNullLabel() {
    std::printf("[T2] null label returns false\n");
    const OfflinePcmEntry table[] = {
        {"你好", kPcmA, sizeof(kPcmA)/sizeof(int16_t), 16000}
    };
    reset_mock();
    EXPECT(PlayOfflineVoice_Ref(nullptr, table, 1) == false, "T2: null label");
    EXPECT(PlayOfflineVoice_Ref("", table, 1) == false, "T2: empty string");
    EXPECT(g_play_invoke_count == 0, "T2: no play() invoked");
}

static void TestExactMatch() {
    std::printf("[T3] exact label match calls play() once with correct args\n");
    const OfflinePcmEntry table[] = {
        {"你好", kPcmA, sizeof(kPcmA)/sizeof(int16_t), 16000},
        {"谢谢", kPcmB, sizeof(kPcmB)/sizeof(int16_t), 22050},
    };
    reset_mock();
    bool r = PlayOfflineVoice_Ref("谢谢", table, 2);
    EXPECT(r == true, "T3: returns true");
    EXPECT(g_play_invoke_count == 1, "T3: play invoked once");
    EXPECT(g_last_play.pcm == kPcmB, "T3: pcm pointer matches entry");
    EXPECT(g_last_play.sample_count == 3, "T3: sample_count matches entry");
    EXPECT(g_last_play.sample_rate == 22050, "T3: sample_rate matches entry");
}

static void TestMissLabel() {
    std::printf("[T4] unknown label returns false, no play() invoked\n");
    const OfflinePcmEntry table[] = {
        {"你好", kPcmA, sizeof(kPcmA)/sizeof(int16_t), 16000}
    };
    reset_mock();
    bool r = PlayOfflineVoice_Ref("救命", table, 1);
    EXPECT(r == false, "T4: unknown -> false");
    EXPECT(g_play_invoke_count == 0, "T4: no play() invoked");
}

static void TestCorruptedEntrySkipped() {
    std::printf("[T5] corrupted entry (null label/data) is skipped, not crashed\n");
    const OfflinePcmEntry table[] = {
        {nullptr, kPcmA, 4, 16000},        // label 空，跳过
        {"你好", nullptr, 4, 16000},       // data 空，跳过
        {"你好", kPcmA, 4, 16000},         // 正常条目，应被命中
    };
    reset_mock();
    bool r = PlayOfflineVoice_Ref("你好", table, 3);
    EXPECT(r == true, "T5: eventually match the valid entry");
    EXPECT(g_play_invoke_count == 1, "T5: play invoked once");
    EXPECT(g_last_play.pcm == kPcmA, "T5: pcm pointer is the valid entry's");
}

static void TestPlayFailurePropagates() {
    std::printf("[T6] play() returning false propagates as PlayOfflineVoice=false\n");
    const OfflinePcmEntry table[] = {
        {"你好", kPcmA, 4, 16000}
    };
    reset_mock();
    g_mock_play_result = false;
    bool r = PlayOfflineVoice_Ref("你好", table, 1);
    EXPECT(r == false, "T6: returns false when play() fails");
    EXPECT(g_play_invoke_count == 1, "T6: play() was invoked (not short-circuited)");
}

static void TestCaseSensitive() {
    std::printf("[T7] matching is case/byte sensitive (strcmp)\n");
    const OfflinePcmEntry table[] = {
        {"HELLO", kPcmA, 4, 16000}
    };
    reset_mock();
    EXPECT(PlayOfflineVoice_Ref("hello", table, 1) == false, "T7: lowercase != uppercase");
    EXPECT(PlayOfflineVoice_Ref("HELLO", table, 1) == true,  "T7: exact uppercase hit");
    EXPECT(g_play_invoke_count == 1, "T7: play invoked only for the exact match");
}

int main() {
    std::printf("========================================\n");
    std::printf(" local_tts_fallback lookup unit tests\n");
    std::printf("========================================\n");

    TestEmptyTable();
    TestNullLabel();
    TestExactMatch();
    TestMissLabel();
    TestCorruptedEntrySkipped();
    TestPlayFailurePropagates();
    TestCaseSensitive();

    std::printf("----------------------------------------\n");
    std::printf(" PASSED: %d   FAILED: %d\n", g_pass, g_fail);
    std::printf("----------------------------------------\n");
    return g_fail == 0 ? 0 : 1;
}
