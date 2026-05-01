#include "llm_client.h"
#include "config.h"
#include "http_client.h"
#include <ArduinoJson.h>

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

static String chatQwen(const char* prompt) {
    const char* url = "https://dashscope.aliyuncs.com/api/v1/services/aigc/text-generation/generation";

    JsonDocument reqDoc;
    reqDoc["model"] = "qwen-turbo";

    JsonObject input = reqDoc["input"].to<JsonObject>();
    JsonArray messages = input["messages"].to<JsonArray>();
    JsonObject msg = messages.add<JsonObject>();
    msg["role"] = "user";
    msg["content"] = prompt;

    JsonObject parameters = reqDoc["parameters"].to<JsonObject>();
    parameters["result_format"] = "message";

    String payload;
    serializeJson(reqDoc, payload);

    String authHeader = "Bearer ";
    authHeader += QWEN_API_KEY;

    String response = httpPostJson(url, payload, authHeader.c_str());
    if (response.length() == 0) {
        return "[错误] 通义千问请求失败";
    }

    JsonDocument respDoc;
    DeserializationError error = deserializeJson(respDoc, response);
    if (error) {
        DEBUG_PRINTLN("[LLM] JSON解析失败");
        return "[错误] 解析阿里响应失败";
    }

    // 新格式: output.choices[0].message.content
    JsonObject output = respDoc["output"];
    if (output) {
        JsonArray choices = output["choices"];
        if (choices && choices.size() > 0) {
            const char* content = choices[0]["message"]["content"];
            if (content) {
                return String(content);
            }
        }
    }

    // 旧格式兼容
    const char* text = respDoc["output"]["text"];
    if (text) {
        return String(text);
    }

    // 检查错误
    const char* errMsg = respDoc["message"];
    if (errMsg) {
        return String("[错误] ") + errMsg;
    }

    return "[错误] 未知响应格式";
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
