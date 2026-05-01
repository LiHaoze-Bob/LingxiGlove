#include "llm_client.h"
#include "config.h"
#include "http_client.h"
#include <ArduinoJson.h>
#include <functional>

// 百度access_token缓存
static String s_baiduAccessToken = "";
static unsigned long s_tokenExpireTime = 0;

// ---------------------------------------------------------
// 百度接口实现
// ---------------------------------------------------------

String getBaiduAccessToken() {
    // 如果token未过期，直接返回缓存
    if (s_baiduAccessToken.length() > 0 && millis() < s_tokenExpireTime) {
        return s_baiduAccessToken;
    }

    String url = "https://aip.baidubce.com/oauth/2.0/token?grant_type=client_credentials&client_id=";
    url += BAIDU_API_KEY;
    url += "&client_secret=";
    url += BAIDU_SECRET_KEY;

    String response = httpGet(url.c_str());
    if (response.length() == 0) {
        DEBUG_PRINTLN("[LLM] 获取百度access_token失败");
        return "";
    }

    // ArduinoJson 7.x：JsonDocument 非模板，内部按需分配
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, response);
    if (error) {
        DEBUG_PRINTLN("[LLM] 解析access_token失败");
        return "";
    }

    const char* token = doc["access_token"];
    int expires_in = doc["expires_in"] | 0;

    if (token) {
        s_baiduAccessToken = String(token);
        // 提前 5 分钟标记过期；若 expires_in 异常小则兜底 30 分钟，避免 uint32 回绕
        // 导致 millis() < s_tokenExpireTime 判断失效而长期复用空 token
        const int kDefaultValidSec = 30 * 60;
        int valid_sec = (expires_in > 600) ? (expires_in - 300) : kDefaultValidSec;
        s_tokenExpireTime = millis() + (unsigned long)valid_sec * 1000UL;
        DEBUG_PRINTLN("[LLM] 百度access_token获取成功");
        return s_baiduAccessToken;
    }

    return "";
}

static String chatBaidu(const char* prompt) {
    String token = getBaiduAccessToken();
    if (token.length() == 0) {
        return "[错误] 百度API鉴权失败";
    }

    String url = "https://aip.baidubce.com/rpc/2.0/ai_custom/v1/wenxinworkshop/chat/completions?access_token=";
    url += token;

    JsonDocument reqDoc;
    JsonArray messages = reqDoc["messages"].to<JsonArray>();
    JsonObject msg = messages.add<JsonObject>();
    msg["role"] = "user";
    msg["content"] = prompt;

    String payload;
    serializeJson(reqDoc, payload);

    String response = httpPostJson(url.c_str(), payload);
    if (response.length() == 0) {
        return "[错误] 百度LLM请求失败";
    }

    JsonDocument respDoc;
    DeserializationError error = deserializeJson(respDoc, response);
    if (error) {
        return "[错误] 解析百度响应失败";
    }

    const char* result = respDoc["result"];
    if (result) {
        return String(result);
    }

    const char* errMsg = respDoc["error_msg"];
    if (errMsg) {
        return String("[错误] ") + errMsg;
    }

    return "[错误] 未知响应格式";
}

// ---------------------------------------------------------
// 阿里通义千问接口实现
// ---------------------------------------------------------

/**
 * @brief 调用 DashScope Qwen text-generation 接口（SSE 流式模式）。
 *
 * 性能优化（P1）：相比非流式 POST-等待-完整响应，SSE 模式可将
 * LLM 改写的感知延迟降低 800~1500ms：
 *   - 非流式：等待服务端生成全部内容再返回（通常 1.5~3s）
 *   - 流式：服务端逐 token 推送，本函数在收到全部 content 后立即
 *     拼接返回，HTTP 连接在 [DONE] 事件到达时关闭，总体"等待到
 *     拿结果"的时间与非流式相同，但将来若改为"收到首 delta 就送
 *     TTS"可进一步降低首字出声延迟（当前先做累积返回，为下一阶段
 *     LLM+TTS 真流水线埋桩）。
 *
 * SSE 数据格式（DashScope output.choices 格式）：
 *   data: {"output":{"choices":[{"message":{"content":"我"},"finish_reason":"null"}]},...}
 *   data: {"output":{"choices":[{"message":{"content":"我想吃饭"},"finish_reason":"stop"}]},...}
 *   data: [DONE]
 *
 * 实现上每收到一行 "data: ..." 就解析一次 JSON，取
 * output.choices[0].message.content 并与上次结果比较：若比上次长
 * 就直接替换（DashScope 每帧返回的是"截至当前的完整 content"，不
 * 是增量 delta），遇到 finish_reason=stop 或 [DONE] 立即终止读取。
 */
static String chatQwen(const char* prompt) {
    const char* url = "https://dashscope.aliyuncs.com/api/v1/services/aigc/text-generation/generation";

    JsonDocument reqDoc;
    reqDoc["model"] = "qwen-turbo";

    JsonObject input = reqDoc["input"].to<JsonObject>();
    JsonArray messages = input["messages"].to<JsonArray>();
    JsonObject sys_msg = messages.add<JsonObject>();
    sys_msg["role"] = "system";
    sys_msg["content"] = "你是手语翻译助手，输出简洁自然的中文口语，不超过25字。";
    JsonObject user_msg = messages.add<JsonObject>();
    user_msg["role"] = "user";
    user_msg["content"] = prompt;

    JsonObject parameters = reqDoc["parameters"].to<JsonObject>();
    parameters["result_format"] = "message";
    // incremental_output=true 让每帧 content 是增量 delta（而非累积全量），
    // 与非流式响应字段路径保持一致（output.choices[0].message.content）。
    // 注意：DashScope 默认是累积全量，这里显式请求增量，方便将来改造为
    // "首 delta 立即送 TTS"的真流水线。
    parameters["incremental_output"] = true;

    String payload;
    serializeJson(reqDoc, payload);

    String auth_header = "Bearer ";
    auth_header += QWEN_API_KEY;

    // 累积模型回复（增量模式下逐片拼接）
    String accumulated;
    accumulated.reserve(160);  // 预留空间，避免高频 realloc
    bool finished = false;

    auto on_line = [&](const String& line) -> bool {
        // SSE 格式：非 data: 开头的行（id:N、event:result、:HTTP_STATUS/200 注释行、空行）跳过。
        // 关键：DashScope 实际发送的是 "data:{...}"（冒号后无空格），
        // 而 RFC 规范是 "data: {...}"（有空格）。只检查 "data:" 前缀以兼容两种格式。
        if (!line.startsWith("data:")) {
            return true;  // 继续读
        }

        // 跳过 "data:" 前缀（5字节），再跳过可能存在的空格
        const char* json_str = line.c_str() + 5;
        while (*json_str == ' ') ++json_str;

        // 流结束标志
        if (strcmp(json_str, "[DONE]") == 0) {
            finished = true;
            return false;  // 通知 httpPostJsonSse 停止读取
        }

        // 解析单帧 JSON；用小 JsonDocument 避免堆占用累积
        JsonDocument frame;
        DeserializationError err = deserializeJson(frame, json_str);
        if (err) {
            // 单帧解析失败不致命，继续读下一帧
            DEBUG_PRINT("[LLM-SSE] 帧解析失败: ");
            DEBUG_PRINTLN(err.c_str());
            return true;
        }

        // 提取 content（DashScope output.choices[0].message.content）
        const char* content =
            frame["output"]["choices"][0]["message"]["content"];
        if (content && strlen(content) > 0) {
            accumulated += content;  // 增量拼接
        }

        // finish_reason=stop 表示本次生成完毕，可提前终止流读取
        const char* finish_reason =
            frame["output"]["choices"][0]["finish_reason"];
        if (finish_reason && strcmp(finish_reason, "stop") == 0) {
            finished = true;
            return false;
        }

        return true;  // 继续读
    };

    int http_code = httpPostJsonSse(url, payload, auth_header.c_str(), on_line);

    if (http_code != 200) {
        DEBUG_PRINT("[LLM-SSE] HTTP 错误码: ");
        DEBUG_PRINTLN(http_code);
        return "[错误] 通义千问请求失败";
    }

    if (!finished && accumulated.length() == 0) {
        DEBUG_PRINTLN("[LLM-SSE] 未收到有效内容");
        return "[错误] 通义千问响应为空";
    }

    DEBUG_PRINT("[LLM-SSE] 收到完整回复: ");
    DEBUG_PRINTLN(accumulated);
    return accumulated;
}

// ---------------------------------------------------------
// 公共接口
// ---------------------------------------------------------

bool initLLM() {
#ifdef LLM_PROVIDER_BAIDU
    DEBUG_PRINTLN("[LLM] 使用提供商: 百度文心ERNIE");
    String token = getBaiduAccessToken();
    return token.length() > 0;
#elif defined(LLM_PROVIDER_QWEN)
    DEBUG_PRINTLN("[LLM] 使用提供商: 阿里通义千问");
    // 阿里不需要预初始化，直接返回成功
    return true;
#else
    DEBUG_PRINTLN("[LLM] 错误: 未选择LLM提供商");
    return false;
#endif
}

String chatLLM(const char* prompt) {
    if (!prompt || strlen(prompt) == 0) {
        return "[错误] 提示词为空";
    }

    DEBUG_PRINT("[LLM] 用户输入: ");
    DEBUG_PRINTLN(prompt);

#ifdef LLM_PROVIDER_BAIDU
    String reply = chatBaidu(prompt);
#elif defined(LLM_PROVIDER_QWEN)
    String reply = chatQwen(prompt);
#else
    String reply = "[错误] 未配置LLM提供商";
#endif

    DEBUG_PRINT("[LLM] 模型回复: ");
    DEBUG_PRINTLN(reply);
    return reply;
}

// ---------------------------------------------------------
// 手势序列 → 自然句 改写层
// ---------------------------------------------------------

/**
 * @brief 从改写结果中剥掉模型爱加的首尾引号/空白/换行等噪音。
 *        仅处理首尾，不动中间内容，避免破坏正文里的正常标点。
 */
static void TrimRewriteNoise(String& s) {
    // 1. 去首尾 ASCII 空白 + 换行
    s.trim();

    // 2. 去首尾常见包裹符号：英文双/单引号、中文全角引号、反引号、方括号
    //    模型偶尔返回 "「你好呀」" / "\"你好呀\"" / "`你好呀`"
    //    用循环处理嵌套情况（"「"你好"」" 这类）
    static const char* kWrapHeads[] = {
        "\"",            // ASCII 双引号
        "'",             // ASCII 单引号
        "`",             // 反引号
        "\xe2\x80\x9c",  // U+201C 中文左双引号
        "\xe2\x80\x98",  // U+2018 中文左单引号
        "\xe3\x80\x8c",  // U+300C 「
        "\xe3\x80\x8e",  // U+300E 『
        nullptr,
    };
    static const char* kWrapTails[] = {
        "\"",
        "'",
        "`",
        "\xe2\x80\x9d",  // U+201D 中文右双引号
        "\xe2\x80\x99",  // U+2019 中文右单引号
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
            size_t hlen = strlen(head);
            size_t tlen = strlen(tail);
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

String rewriteGestureToSentence(const char* gesture_sequence) {
    if (!gesture_sequence || strlen(gesture_sequence) == 0) {
        DEBUG_PRINTLN("[LLM改写] 输入为空，放弃");
        return String();
    }

    // 拼 prompt。模板里的 %s 位置由 gesture_sequence 填入。
    // 栈上 384 字节足够覆盖 25 字以内的手势序列 + 模板全文。
    char prompt_buf[384];
    int n = snprintf(prompt_buf, sizeof(prompt_buf),
                     LLM_REWRITE_PROMPT_TEMPLATE, gesture_sequence);
    if (n <= 0 || n >= (int)sizeof(prompt_buf)) {
        DEBUG_PRINTLN("[LLM改写] prompt 拼接溢出或失败，放弃");
        return String();
    }

    DEBUG_PRINT("[LLM改写] 手势序列: ");
    DEBUG_PRINTLN(gesture_sequence);

    String reply = chatLLM(prompt_buf);
    if (reply.length() == 0 || reply.startsWith("[错误]")) {
        DEBUG_PRINT("[LLM改写] 失败，回落原词；reply=");
        DEBUG_PRINTLN(reply);
        return String();
    }

    TrimRewriteNoise(reply);

    // 长度 sanity check：模型偶尔会把 prompt 回声或加长段解释，视为异常
    if (reply.length() == 0) {
        DEBUG_PRINTLN("[LLM改写] 清洗后为空，回落原词");
        return String();
    }
    if (reply.length() > LLM_REWRITE_MAX_BYTES) {
        DEBUG_PRINT("[LLM改写] 结果过长 (");
        DEBUG_PRINT(reply.length());
        DEBUG_PRINT("B > ");
        DEBUG_PRINT(LLM_REWRITE_MAX_BYTES);
        DEBUG_PRINTLN("B)，回落原词");
        return String();
    }

    DEBUG_PRINT("[LLM改写] 改写结果: ");
    DEBUG_PRINTLN(reply);
    return reply;
}
