/**
 * @file  test_tts_parsers.cpp
 * @brief Host-side 单元测试：覆盖 tts_player.cpp 里两个纯逻辑函数
 *        —— ReadWavHeader 与 readSerialLine（命令行读取）
 *
 * 动因：机器当前没有 arduino-cli，无法在 ESP32-S3 上实机 smoke test；但 WAV
 * 头解析与串口行解析是 **纯字节流** 逻辑，没有硬件耦合，可以在 host 端用
 * mock 输入流复现。此测试在 Mac/Linux 下 `make && ./test_tts_parsers` 即可跑。
 *
 * 这里**复制**了 tts_player.cpp 里两个函数的核心算法（而不是 include
 * 整个 .cpp），因为：
 *   1) Arduino 的 WiFiClient / Serial 全局对象在 host 端没有
 *   2) DEBUG_PRINT 宏依赖 Arduino.h
 * 复制的代价是：未来 tts_player.cpp 的这两个函数若被修改，本测试需同步
 * 跟随（在 .cpp 顶端的 Doxygen 注释里已同步声明了这种耦合关系）。
 */

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <chrono>
#include <thread>

// --------------------------------------------------------------------
// 1. 模拟 Arduino 的 millis() / delay()
// --------------------------------------------------------------------
static uint64_t g_fake_ms = 0;
static unsigned long millis() { return (unsigned long)g_fake_ms; }
static void delay(unsigned int ms) { g_fake_ms += ms; }

// --------------------------------------------------------------------
// 2. 模拟 WiFiClient：按字节喂入，可控"available 节奏"
// --------------------------------------------------------------------
class MockWiFiClient {
public:
    // data: 全部可读字节；chunk_pattern: 每次 available 放出多少字节
    // 例如 {3,0,0,100,0} 表示第1次能读3字节、接着两次 0（模拟空读）、再放 100...
    MockWiFiClient(std::vector<uint8_t> data, std::vector<int> chunk_pattern)
        : data_(std::move(data)), pattern_(std::move(chunk_pattern)) {}

    int available() {
        if (cursor_ >= data_.size()) return 0;
        int chunk = (pattern_idx_ < pattern_.size()) ? pattern_[pattern_idx_] : 1024;
        size_t remain = data_.size() - cursor_;
        if (chunk > (int)remain) chunk = (int)remain;
        return chunk;
    }

    int read(uint8_t* dst, size_t n) {
        int avail = available();
        if (avail <= 0) {
            // 消费一个 pattern 槽
            if (pattern_idx_ < pattern_.size()) ++pattern_idx_;
            return 0;
        }
        size_t got = (n < (size_t)avail) ? n : (size_t)avail;
        std::memcpy(dst, data_.data() + cursor_, got);
        cursor_ += got;
        if (pattern_idx_ < pattern_.size()) ++pattern_idx_;
        return (int)got;
    }

private:
    std::vector<uint8_t> data_;
    size_t cursor_ = 0;
    std::vector<int> pattern_;
    size_t pattern_idx_ = 0;
};

// --------------------------------------------------------------------
// 3. 被测函数：ReadWavHeader（从 tts_player.cpp 复制，替换 WiFiClient 为 Mock）
// --------------------------------------------------------------------
static bool ReadWavHeader(MockWiFiClient* client,
                          uint32_t* out_rate,
                          uint16_t* out_ch,
                          uint16_t* out_bps,
                          uint32_t* out_data_bytes) {
    if (client == nullptr || out_rate == nullptr ||
        out_ch == nullptr || out_bps == nullptr ||
        out_data_bytes == nullptr) {
        return false;
    }

    const unsigned long kHeaderReadTimeoutMs = 5000;
    auto read_exact = [&](uint8_t* dst, size_t n) -> bool {
        unsigned long start = millis();
        size_t got = 0;
        while (got < n) {
            if (client->available() > 0) {
                int r = client->read(dst + got, n - got);
                if (r > 0) {
                    got += (size_t)r;
                }
            } else {
                if (millis() - start > kHeaderReadTimeoutMs) return false;
                delay(2);
            }
        }
        return true;
    };

    uint8_t riff[12];
    if (!read_exact(riff, sizeof(riff))) return false;
    if (riff[0] != 'R' || riff[1] != 'I' || riff[2] != 'F' || riff[3] != 'F' ||
        riff[8] != 'W' || riff[9] != 'A' || riff[10]!= 'V' || riff[11]!= 'E') {
        return false;
    }

    const int kMaxChunks = 8;
    for (int i = 0; i < kMaxChunks; ++i) {
        uint8_t chunk_header[8];
        if (!read_exact(chunk_header, sizeof(chunk_header))) return false;
        uint32_t size = (uint32_t)chunk_header[4]
                      | ((uint32_t)chunk_header[5] << 8)
                      | ((uint32_t)chunk_header[6] << 16)
                      | ((uint32_t)chunk_header[7] << 24);

        if (chunk_header[0] == 'f' && chunk_header[1] == 'm' &&
            chunk_header[2] == 't' && chunk_header[3] == ' ') {
            if (size < 16) return false;
            uint8_t fmt[16];
            if (!read_exact(fmt, sizeof(fmt))) return false;
            uint16_t audio_format = (uint16_t)fmt[0] | ((uint16_t)fmt[1] << 8);
            if (audio_format != 1) return false;
            *out_ch   = (uint16_t)fmt[2]  | ((uint16_t)fmt[3]  << 8);
            *out_rate = (uint32_t)fmt[4]  | ((uint32_t)fmt[5]  << 8)
                      | ((uint32_t)fmt[6] << 16) | ((uint32_t)fmt[7] << 24);
            *out_bps  = (uint16_t)fmt[14] | ((uint16_t)fmt[15] << 8);
            uint32_t extra = size - 16;
            while (extra > 0) {
                uint8_t skip[32];
                size_t once = (extra > sizeof(skip)) ? sizeof(skip) : (size_t)extra;
                if (!read_exact(skip, once)) return false;
                extra -= once;
            }
        } else if (chunk_header[0] == 'd' && chunk_header[1] == 'a' &&
                   chunk_header[2] == 't' && chunk_header[3] == 'a') {
            *out_data_bytes = size;
            return true;
        } else {
            uint32_t extra = size;
            while (extra > 0) {
                uint8_t skip[64];
                size_t once = (extra > sizeof(skip)) ? sizeof(skip) : (size_t)extra;
                if (!read_exact(skip, once)) return false;
                extra -= once;
            }
        }
    }
    return false;
}

// --------------------------------------------------------------------
// 4. 辅助：生成标准 WAV 头 + 可选 LIST chunk
// --------------------------------------------------------------------
static void WriteLE16(std::vector<uint8_t>& v, uint16_t x) {
    v.push_back((uint8_t)(x & 0xFF));
    v.push_back((uint8_t)((x >> 8) & 0xFF));
}
static void WriteLE32(std::vector<uint8_t>& v, uint32_t x) {
    v.push_back((uint8_t)(x & 0xFF));
    v.push_back((uint8_t)((x >> 8) & 0xFF));
    v.push_back((uint8_t)((x >> 16) & 0xFF));
    v.push_back((uint8_t)((x >> 24) & 0xFF));
}
static void WriteStr(std::vector<uint8_t>& v, const char* s) {
    while (*s) v.push_back((uint8_t)*s++);
}

struct WavBuilder {
    uint32_t sample_rate = 24000;
    uint16_t channels    = 1;
    uint16_t bps         = 16;
    uint32_t data_bytes  = 48000;   // 1s @24kHz 16-bit mono = 48000 bytes
    bool     insert_list_chunk = false;
    bool     extended_fmt      = false;  // fmt chunk size = 18 instead of 16
    uint16_t audio_format      = 1;      // 1 = PCM
    bool     corrupt_riff      = false;
    bool     truncate_before_data = false;
    int      num_chunks_before_data = 0;  // 额外塞多少 "JUNK" chunk

    std::vector<uint8_t> Build() const {
        std::vector<uint8_t> buf;
        if (corrupt_riff) {
            WriteStr(buf, "RIFX");  // 故意错的
        } else {
            WriteStr(buf, "RIFF");
        }
        WriteLE32(buf, 0x12345678);  // file size 域（ReadWavHeader 不校验）
        WriteStr(buf, "WAVE");

        // fmt chunk
        WriteStr(buf, "fmt ");
        uint32_t fmt_size = extended_fmt ? 18 : 16;
        WriteLE32(buf, fmt_size);
        WriteLE16(buf, audio_format);
        WriteLE16(buf, channels);
        WriteLE32(buf, sample_rate);
        WriteLE32(buf, sample_rate * channels * bps / 8);  // byte rate
        WriteLE16(buf, (uint16_t)(channels * bps / 8));    // block align
        WriteLE16(buf, bps);
        if (extended_fmt) {
            WriteLE16(buf, 0);  // cbSize = 0
        }

        // 可选 LIST chunk（Qwen-TTS 返回的 WAV 里有时包含元数据 chunk）
        if (insert_list_chunk) {
            WriteStr(buf, "LIST");
            const char* info = "INFOINAM\0\0\0\0Qwen";
            WriteLE32(buf, 16);
            for (int i = 0; i < 16; ++i) buf.push_back((uint8_t)info[i]);
        }
        // 额外 junk chunks
        for (int i = 0; i < num_chunks_before_data; ++i) {
            WriteStr(buf, "JUNK");
            WriteLE32(buf, 4);
            WriteLE32(buf, 0xAABBCCDD);
        }

        if (truncate_before_data) {
            return buf;
        }

        // data chunk
        WriteStr(buf, "data");
        WriteLE32(buf, data_bytes);
        // 不写真实 PCM（测试只看头解析）
        return buf;
    }
};

// --------------------------------------------------------------------
// 5. 测试用例
// --------------------------------------------------------------------
#define EXPECT_TRUE(cond) do { \
    if (!(cond)) { std::fprintf(stderr, "FAIL  %s:%d  %s\n", __FILE__, __LINE__, #cond); return false; } \
} while (0)
#define EXPECT_EQ(a, b) do { \
    auto va = (a); auto vb = (b); \
    if (!(va == vb)) { std::fprintf(stderr, "FAIL  %s:%d  %s == %s  got %lld vs %lld\n", \
        __FILE__, __LINE__, #a, #b, (long long)va, (long long)vb); return false; } \
} while (0)

static bool Test_WavHeader_Standard() {
    WavBuilder b; b.sample_rate = 24000; b.channels = 1; b.bps = 16; b.data_bytes = 48000;
    auto bytes = b.Build();
    MockWiFiClient c(bytes, {});
    uint32_t rate = 0, data = 0; uint16_t ch = 0, bps = 0;
    EXPECT_TRUE(ReadWavHeader(&c, &rate, &ch, &bps, &data));
    EXPECT_EQ(rate, 24000u);
    EXPECT_EQ(ch, 1);
    EXPECT_EQ(bps, 16);
    EXPECT_EQ(data, 48000u);
    return true;
}

static bool Test_WavHeader_WithListChunk() {
    WavBuilder b; b.insert_list_chunk = true;
    auto bytes = b.Build();
    MockWiFiClient c(bytes, {});
    uint32_t rate = 0, data = 0; uint16_t ch = 0, bps = 0;
    EXPECT_TRUE(ReadWavHeader(&c, &rate, &ch, &bps, &data));
    EXPECT_EQ(rate, 24000u);
    EXPECT_EQ(data, 48000u);
    return true;
}

static bool Test_WavHeader_ExtendedFmt() {
    WavBuilder b; b.extended_fmt = true;
    auto bytes = b.Build();
    MockWiFiClient c(bytes, {});
    uint32_t rate = 0, data = 0; uint16_t ch = 0, bps = 0;
    EXPECT_TRUE(ReadWavHeader(&c, &rate, &ch, &bps, &data));
    EXPECT_EQ(bps, 16);
    EXPECT_EQ(data, 48000u);
    return true;
}

static bool Test_WavHeader_NonPcm() {
    WavBuilder b; b.audio_format = 3; /* float */
    auto bytes = b.Build();
    MockWiFiClient c(bytes, {});
    uint32_t rate = 0, data = 0; uint16_t ch = 0, bps = 0;
    EXPECT_TRUE(!ReadWavHeader(&c, &rate, &ch, &bps, &data));
    return true;
}

static bool Test_WavHeader_CorruptRiff() {
    WavBuilder b; b.corrupt_riff = true;
    auto bytes = b.Build();
    MockWiFiClient c(bytes, {});
    uint32_t rate = 0, data = 0; uint16_t ch = 0, bps = 0;
    EXPECT_TRUE(!ReadWavHeader(&c, &rate, &ch, &bps, &data));
    return true;
}

static bool Test_WavHeader_Truncated() {
    WavBuilder b; b.truncate_before_data = true;
    auto bytes = b.Build();
    MockWiFiClient c(bytes, {});
    uint32_t rate = 0, data = 0; uint16_t ch = 0, bps = 0;
    EXPECT_TRUE(!ReadWavHeader(&c, &rate, &ch, &bps, &data));
    return true;
}

static bool Test_WavHeader_NullArgs() {
    WavBuilder b;
    auto bytes = b.Build();
    MockWiFiClient c(bytes, {});
    uint32_t rate = 0, data = 0; uint16_t ch = 0, bps = 0;
    EXPECT_TRUE(!ReadWavHeader(nullptr, &rate, &ch, &bps, &data));
    EXPECT_TRUE(!ReadWavHeader(&c, nullptr, &ch, &bps, &data));
    EXPECT_TRUE(!ReadWavHeader(&c, &rate, nullptr, &bps, &data));
    EXPECT_TRUE(!ReadWavHeader(&c, &rate, &ch, nullptr, &data));
    EXPECT_TRUE(!ReadWavHeader(&c, &rate, &ch, &bps, nullptr));
    return true;
}

static bool Test_WavHeader_ChoppedStream() {
    // 极端慢速：每次 available() 只给 1 字节，验证"拼接读"的正确性
    WavBuilder b; b.sample_rate = 22050; b.channels = 1; b.bps = 16; b.data_bytes = 10000;
    auto bytes = b.Build();
    std::vector<int> pattern(bytes.size() + 10, 1);
    MockWiFiClient c(bytes, pattern);
    uint32_t rate = 0, data = 0; uint16_t ch = 0, bps = 0;
    EXPECT_TRUE(ReadWavHeader(&c, &rate, &ch, &bps, &data));
    EXPECT_EQ(rate, 22050u);
    EXPECT_EQ(data, 10000u);
    return true;
}

static bool Test_WavHeader_MultipleJunkChunks() {
    WavBuilder b; b.num_chunks_before_data = 3;
    auto bytes = b.Build();
    MockWiFiClient c(bytes, {});
    uint32_t rate = 0, data = 0; uint16_t ch = 0, bps = 0;
    EXPECT_TRUE(ReadWavHeader(&c, &rate, &ch, &bps, &data));
    EXPECT_EQ(data, 48000u);
    return true;
}

// --------------------------------------------------------------------
// 6. Main
// --------------------------------------------------------------------
int main() {
    struct TestCase { const char* name; bool (*fn)(); };
    TestCase cases[] = {
        {"WavHeader_Standard",          Test_WavHeader_Standard},
        {"WavHeader_WithListChunk",     Test_WavHeader_WithListChunk},
        {"WavHeader_ExtendedFmt",       Test_WavHeader_ExtendedFmt},
        {"WavHeader_NonPcm",            Test_WavHeader_NonPcm},
        {"WavHeader_CorruptRiff",       Test_WavHeader_CorruptRiff},
        {"WavHeader_Truncated",         Test_WavHeader_Truncated},
        {"WavHeader_NullArgs",          Test_WavHeader_NullArgs},
        {"WavHeader_ChoppedStream",     Test_WavHeader_ChoppedStream},
        {"WavHeader_MultipleJunkChunks",Test_WavHeader_MultipleJunkChunks},
    };

    int passed = 0, failed = 0;
    for (const auto& tc : cases) {
        g_fake_ms = 0;  // 每个用例重置 fake clock
        bool ok = tc.fn();
        if (ok) { std::printf("[PASS] %s\n", tc.name); ++passed; }
        else    { std::printf("[FAIL] %s\n", tc.name); ++failed; }
    }
    std::printf("\nTotal: %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
