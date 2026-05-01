/**
 * @file  test_llm_rewrite.cpp
 * @brief Host-side 单元测试：覆盖 llm_client.cpp 里的纯逻辑 TrimRewriteNoise
 *
 * 动因：rewriteGestureToSentence 的网络/鉴权部分无法在 host 端直接跑，但
 *       其中的"引号/空白去噪"逻辑 (TrimRewriteNoise) 是纯字符串处理，可以
 *       在 host 端用 Arduino String 的 shim 完整重放，避免 LLM 真的把前后
 *       包裹了引号 / 方括号 / 换行时，播报出现「"你好呀"」这种脏文本。
 *
 * 复制策略：与 test_tts_parsers 保持一致 —— **复制** llm_client.cpp 里
 * TrimRewriteNoise 的算法，避免拖入整个 Arduino 运行时；未来源文件变更
 * 时本测试需同步跟随（在 llm_client.cpp 的 Doxygen 里有耦合声明）。
 *
 * 用法：make && ./test_llm_rewrite
 */

#include <cassert>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>

// --------------------------------------------------------------------
// 1. Arduino String 最小 shim（仅覆盖 TrimRewriteNoise 用到的 API）
// --------------------------------------------------------------------
class String {
public:
    String() = default;
    String(const char* s) : s_(s ? s : "") {}
    String(const std::string& s) : s_(s) {}

    size_t length() const { return s_.size(); }

    void trim() {
        size_t a = 0, b = s_.size();
        while (a < b && (s_[a] == ' ' || s_[a] == '\t' ||
                         s_[a] == '\r' || s_[a] == '\n')) ++a;
        while (b > a && (s_[b-1] == ' ' || s_[b-1] == '\t' ||
                         s_[b-1] == '\r' || s_[b-1] == '\n')) --b;
        s_ = s_.substr(a, b - a);
    }

    bool startsWith(const char* p) const {
        size_t n = std::strlen(p);
        return s_.size() >= n && std::memcmp(s_.data(), p, n) == 0;
    }

    bool endsWith(const char* p) const {
        size_t n = std::strlen(p);
        return s_.size() >= n &&
               std::memcmp(s_.data() + s_.size() - n, p, n) == 0;
    }

    String substring(size_t a, size_t b) const {
        if (b <= a || a >= s_.size()) return String();
        if (b > s_.size()) b = s_.size();
        return String(s_.substr(a, b - a));
    }

    const char* c_str() const { return s_.c_str(); }
    bool operator==(const char* rhs) const { return s_ == (rhs ? rhs : ""); }

private:
    std::string s_;
};

// --------------------------------------------------------------------
// 2. 被测函数（从 llm_client.cpp 复制）
// --------------------------------------------------------------------
static void TrimRewriteNoise(String& s) {
    s.trim();

    static const char* kWrapHeads[] = {
        "\"",
        "'",
        "`",
        "\xe2\x80\x9c",  // U+201C "
        "\xe2\x80\x98",  // U+2018 '
        "\xe3\x80\x8c",  // U+300C 「
        "\xe3\x80\x8e",  // U+300E 『
        nullptr,
    };
    static const char* kWrapTails[] = {
        "\"",
        "'",
        "`",
        "\xe2\x80\x9d",  // U+201D "
        "\xe2\x80\x99",  // U+2019 '
        "\xe3\x80\x8d",  // U+300D 」
        "\xe3\x80\x8f",  // U+300F 』
        nullptr,
    };

    bool changed = true;
    while (changed && s.length() > 0) {
        changed = false;
        for (int i = 0; kWrapHeads[i] != nullptr; i++) {
            const char* head = kWrapHeads[i];
            const char* tail = kWrapTails[i];
            size_t hlen = std::strlen(head);
            size_t tlen = std::strlen(tail);
            if (s.length() >= hlen + tlen &&
                s.startsWith(head) && s.endsWith(tail)) {
                s = s.substring(hlen, s.length() - tlen);
                s.trim();
                changed = true;
                break;
            }
        }
    }
}

// --------------------------------------------------------------------
// 3. 测试宏
// --------------------------------------------------------------------
#define EXPECT_EQ_STR(actual_str, expected_cstr)                       \
    do {                                                                \
        String _a = (actual_str);                                       \
        if (!(_a == (expected_cstr))) {                                 \
            std::printf("  FAIL  expected=\"%s\" actual=\"%s\"\n",      \
                        (expected_cstr), _a.c_str());                   \
            return false;                                               \
        }                                                               \
    } while (0)

// --------------------------------------------------------------------
// 4. 用例
// --------------------------------------------------------------------
static bool Test_CleanAlreadyClean() {
    String s("你好呀");
    TrimRewriteNoise(s);
    EXPECT_EQ_STR(s, "你好呀");
    return true;
}

static bool Test_StripLeadingTrailingSpaces() {
    String s("  我想吃饭  ");
    TrimRewriteNoise(s);
    EXPECT_EQ_STR(s, "我想吃饭");
    return true;
}

static bool Test_StripTrailingNewlines() {
    String s("我想吃饭\r\n");
    TrimRewriteNoise(s);
    EXPECT_EQ_STR(s, "我想吃饭");
    return true;
}

static bool Test_StripAsciiDoubleQuotes() {
    String s("\"你好呀\"");
    TrimRewriteNoise(s);
    EXPECT_EQ_STR(s, "你好呀");
    return true;
}

static bool Test_StripAsciiSingleQuotes() {
    String s("'你好呀'");
    TrimRewriteNoise(s);
    EXPECT_EQ_STR(s, "你好呀");
    return true;
}

static bool Test_StripBackticks() {
    String s("`你好呀`");
    TrimRewriteNoise(s);
    EXPECT_EQ_STR(s, "你好呀");
    return true;
}

static bool Test_StripChineseDoubleQuotes() {
    // U+201C 你好呀 U+201D
    String s("\xe2\x80\x9c""你好呀""\xe2\x80\x9d");
    TrimRewriteNoise(s);
    EXPECT_EQ_STR(s, "你好呀");
    return true;
}

static bool Test_StripChineseBracketQuotes() {
    // U+300C 「 你好呀 U+300D 」
    String s("\xe3\x80\x8c""你好呀""\xe3\x80\x8d");
    TrimRewriteNoise(s);
    EXPECT_EQ_STR(s, "你好呀");
    return true;
}

static bool Test_StripNestedQuotes() {
    // 「"你好呀"」 —— 先剥「」再剥 ""
    String s("\xe3\x80\x8c""\"你好呀\"""\xe3\x80\x8d");
    TrimRewriteNoise(s);
    EXPECT_EQ_STR(s, "你好呀");
    return true;
}

static bool Test_StripQuotesWithInnerSpaces() {
    String s("\" 我想吃饭 \"");
    TrimRewriteNoise(s);
    EXPECT_EQ_STR(s, "我想吃饭");
    return true;
}

static bool Test_DoNotStripInnerQuotes() {
    // 内部引号应保留：他说"你好"，不应被去掉
    // 这里测试首尾没有包裹，中间有引号
    String s("他说\"你好\"");
    TrimRewriteNoise(s);
    EXPECT_EQ_STR(s, "他说\"你好\"");
    return true;
}

static bool Test_EmptyInput() {
    String s("");
    TrimRewriteNoise(s);
    EXPECT_EQ_STR(s, "");
    return true;
}

static bool Test_OnlyWhitespace() {
    String s("   \r\n  ");
    TrimRewriteNoise(s);
    EXPECT_EQ_STR(s, "");
    return true;
}

static bool Test_AsymmetricQuoteNotStripped() {
    // 只有左引号没有右引号：不应剥离（保守策略，避免把正文吃掉）
    String s("\"你好呀");
    TrimRewriteNoise(s);
    EXPECT_EQ_STR(s, "\"你好呀");
    return true;
}

static bool Test_MixedNewlineQuoteNoise() {
    // 真实 LLM 输出风格：前后带换行 + 中文引号
    String s("\n\xe3\x80\x8c""我想吃饭""\xe3\x80\x8d\n");
    TrimRewriteNoise(s);
    EXPECT_EQ_STR(s, "我想吃饭");
    return true;
}

// --------------------------------------------------------------------
// 5. Main
// --------------------------------------------------------------------
int main() {
    struct TestCase { const char* name; bool (*fn)(); };
    TestCase cases[] = {
        {"CleanAlreadyClean",          Test_CleanAlreadyClean},
        {"StripLeadingTrailingSpaces", Test_StripLeadingTrailingSpaces},
        {"StripTrailingNewlines",      Test_StripTrailingNewlines},
        {"StripAsciiDoubleQuotes",     Test_StripAsciiDoubleQuotes},
        {"StripAsciiSingleQuotes",     Test_StripAsciiSingleQuotes},
        {"StripBackticks",             Test_StripBackticks},
        {"StripChineseDoubleQuotes",   Test_StripChineseDoubleQuotes},
        {"StripChineseBracketQuotes",  Test_StripChineseBracketQuotes},
        {"StripNestedQuotes",          Test_StripNestedQuotes},
        {"StripQuotesWithInnerSpaces", Test_StripQuotesWithInnerSpaces},
        {"DoNotStripInnerQuotes",      Test_DoNotStripInnerQuotes},
        {"EmptyInput",                 Test_EmptyInput},
        {"OnlyWhitespace",             Test_OnlyWhitespace},
        {"AsymmetricQuoteNotStripped", Test_AsymmetricQuoteNotStripped},
        {"MixedNewlineQuoteNoise",     Test_MixedNewlineQuoteNoise},
    };

    int passed = 0, failed = 0;
    for (const auto& tc : cases) {
        bool ok = tc.fn();
        if (ok) { std::printf("[PASS] %s\n", tc.name); ++passed; }
        else    { std::printf("[FAIL] %s\n", tc.name); ++failed; }
    }
    std::printf("\nTotal: %d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
