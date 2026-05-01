#include "http_client.h"
#include "config.h"

#include <WiFiClientSecure.h>

/**
 * @brief 单次 HTTP/HTTPS 请求的默认超时（毫秒）。
 *
 * DashScope Qwen-TTS 在国内主站典型 req-cost-time 约 800-1500 ms，
 * ESP32-S3 的 TLS 握手再消耗 1-2 s，叠加 WiFi 信号在 -70 dBm 附近的重传，
 * 总耗时可能逼近 5 s。HTTPClient 默认 readTimeout 偏短，生产链路
 * 必须显式拉长，避免合成成功但读取阶段超时（表现为 "read Timeout"）。
 */
static const uint16_t kHttpTimeoutMs = 15000;

/**
 * @brief 判断给定 URL 是否为 HTTPS。
 *
 * 用于决定走 HTTP 纯文本客户端还是 WiFiClientSecure（TLS）。
 */
static bool IsHttpsUrl(const char* url) {
    if (url == nullptr) {
        return false;
    }
    return (strncmp(url, "https://", 8) == 0);
}

/**
 * @brief 以 HTTPS 方式打开 HTTPClient 连接（skip 证书校验 + 拉长超时）。
 *
 * 选择 setInsecure() 的工程权衡：
 *   - DashScope 证书链由 GlobalSign 签发，可能随时轮换；在 ESP32-S3
 *     上硬编码根证书一旦到期即链路断裂；
 *   - WiFiClientSecure 的证书校验每次握手额外 40-60 KB RAM，
 *     而本项目 SRAM 已被 I2S/JsonDocument 吃紧；
 *   - 本链路仅传文本 + 下载 TTS 音频 URL（短文本），中间人风险可接受。
 *
 * 调用方对 secure_client 的生命周期负责：HTTPClient 持有其引用，
 * 因此 secure_client 必须与 HTTPClient 同生命周期（栈上持有即可）。
 */
static bool BeginHttps(HTTPClient& http, WiFiClientSecure& secure_client, const char* url) {
    secure_client.setInsecure();
    secure_client.setTimeout(kHttpTimeoutMs / 1000);  // WiFiClient::setTimeout 单位为秒
    return http.begin(secure_client, url);
}

String httpPostJson(const char* url, const String& jsonPayload, const char* authHeader) {
    if (url == nullptr || url[0] == '\0') {
        DEBUG_PRINTLN("[HTTP] 错误: url 为空");
        return String();
    }

    HTTPClient http;
    WiFiClientSecure secure_client;  // 仅 HTTPS 分支用到，放外层保证生命周期

    bool begin_ok = false;
    if (IsHttpsUrl(url)) {
        begin_ok = BeginHttps(http, secure_client, url);
    } else {
        begin_ok = http.begin(url);
    }
    if (!begin_ok) {
        DEBUG_PRINT("[HTTP] begin 失败: ");
        DEBUG_PRINTLN(url);
        return String();
    }

    http.setTimeout(kHttpTimeoutMs);
    http.setConnectTimeout(kHttpTimeoutMs);
    http.addHeader("Content-Type", "application/json");

    if (authHeader != nullptr) {
        http.addHeader("Authorization", authHeader);
    }

    DEBUG_PRINT("[HTTP] POST ");
    DEBUG_PRINTLN(url);

    int httpCode = http.POST(jsonPayload);
    String response;

    if (httpCode > 0) {
        DEBUG_PRINT("[HTTP] 状态码: ");
        DEBUG_PRINTLN(httpCode);
        response = http.getString();
    } else {
        DEBUG_PRINT("[HTTP] 请求失败, 错误: ");
        DEBUG_PRINTLN(http.errorToString(httpCode));
    }

    http.end();
    return response;
}

int httpPostJsonSse(const char* url,
                    const String& jsonPayload,
                    const char* authHeader,
                    std::function<bool(const String& line)> onLine) {
    if (url == nullptr || url[0] == '\0') {
        DEBUG_PRINTLN("[HTTP-SSE] 错误: url 为空");
        return -1;
    }
    if (!onLine) {
        DEBUG_PRINTLN("[HTTP-SSE] 错误: onLine 回调为空");
        return -1;
    }

    HTTPClient http;
    WiFiClientSecure secure_client;

    bool begin_ok = false;
    if (IsHttpsUrl(url)) {
        begin_ok = BeginHttps(http, secure_client, url);
    } else {
        begin_ok = http.begin(url);
    }
    if (!begin_ok) {
        DEBUG_PRINT("[HTTP-SSE] begin 失败: ");
        DEBUG_PRINTLN(url);
        return -1;
    }

    http.setTimeout(kHttpTimeoutMs);
    http.setConnectTimeout(kHttpTimeoutMs);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Accept", "text/event-stream");
    http.addHeader("X-DashScope-SSE", "enable");
    if (authHeader != nullptr) {
        http.addHeader("Authorization", authHeader);
    }

    DEBUG_PRINT("[HTTP-SSE] POST ");
    DEBUG_PRINTLN(url);

    int http_code = http.POST(jsonPayload);
    if (http_code <= 0) {
        DEBUG_PRINT("[HTTP-SSE] 请求失败: ");
        DEBUG_PRINTLN(http.errorToString(http_code));
        http.end();
        return http_code;
    }

    DEBUG_PRINT("[HTTP-SSE] 状态码: ");
    DEBUG_PRINTLN(http_code);

    if (http_code != HTTP_CODE_OK) {
        // 非 200 时把错误体打印出来帮助调试（不超过 256 字节）
        WiFiClient* err_stream = http.getStreamPtr();
        if (err_stream) {
            char err_buf[257] = {};
            int got = 0;
            unsigned long deadline = millis() + 3000UL;
            while (got < 256 && millis() < deadline) {
                if (err_stream->available()) {
                    err_buf[got++] = (char)err_stream->read();
                } else {
                    delay(5);
                }
            }
            err_buf[got] = '\0';
            DEBUG_PRINT("[HTTP-SSE] 错误体: ");
            DEBUG_PRINTLN(err_buf);
        }
        http.end();
        return http_code;
    }

    WiFiClient* stream = http.getStreamPtr();
    if (!stream) {
        DEBUG_PRINTLN("[HTTP-SSE] 无法获取流指针");
        http.end();
        return -1;
    }

    // 逐字节读取，以 '\n' 为行分隔符，组装完整行后交给 onLine 回调。
    // 嵌入式场景下避免 String 在高频循环里动态扩容：使用固定栈缓冲区，
    // 超长行（> 511 字节）截断处理（SSE data 行通常 < 300 字节）。
    static char s_line_buf[512];
    int line_len = 0;
    const unsigned long kReadTimeout = 20000UL;  // 单次流读取总超时
    unsigned long deadline = millis() + kReadTimeout;
    bool stop_early = false;

    while (!stop_early && millis() < deadline) {
        if (!stream->available()) {
            if (!http.connected()) break;
            delay(2);
            continue;
        }
        // 更新 deadline：只要还在收数据就续期，避免大模型思考时被切断
        deadline = millis() + kReadTimeout;

        int ch = stream->read();
        if (ch < 0) continue;

        if (ch == '\n') {
            // 去掉末尾的 '\r'（Windows 风格行尾）
            if (line_len > 0 && s_line_buf[line_len - 1] == '\r') {
                --line_len;
            }
            s_line_buf[line_len] = '\0';

            if (line_len > 0) {
                // 把 C 字符串包成 Arduino String 交给回调；
                // 回调返回 false 时提前终止读取流
                String line_str(s_line_buf);
                if (!onLine(line_str)) {
                    stop_early = true;
                }
            }
            line_len = 0;
        } else {
            if (line_len < (int)sizeof(s_line_buf) - 1) {
                s_line_buf[line_len++] = (char)ch;
            }
            // 溢出：截断，等下一个 '\n' 再重置
        }
    }

    http.end();
    return http_code;
}

String httpGet(const char* url, const char* authHeader) {
    if (url == nullptr || url[0] == '\0') {
        DEBUG_PRINTLN("[HTTP] 错误: url 为空");
        return String();
    }

    HTTPClient http;
    WiFiClientSecure secure_client;

    bool begin_ok = false;
    if (IsHttpsUrl(url)) {
        begin_ok = BeginHttps(http, secure_client, url);
    } else {
        begin_ok = http.begin(url);
    }
    if (!begin_ok) {
        DEBUG_PRINT("[HTTP] begin 失败: ");
        DEBUG_PRINTLN(url);
        return String();
    }

    http.setTimeout(kHttpTimeoutMs);
    http.setConnectTimeout(kHttpTimeoutMs);

    if (authHeader != nullptr) {
        http.addHeader("Authorization", authHeader);
    }

    DEBUG_PRINT("[HTTP] GET ");
    DEBUG_PRINTLN(url);

    int httpCode = http.GET();
    String response;

    if (httpCode > 0) {
        DEBUG_PRINT("[HTTP] 状态码: ");
        DEBUG_PRINTLN(httpCode);
        response = http.getString();
    } else {
        DEBUG_PRINT("[HTTP] 请求失败, 错误: ");
        DEBUG_PRINTLN(http.errorToString(httpCode));
    }

    http.end();
    return response;
}

String urlEncode(const char* str) {
    String encoded = "";
    char c;
    while ((c = *str++) != '\0') {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            encoded += c;
        } else {
            char buf[4];
            sprintf(buf, "%%%02X", (unsigned char)c);
            encoded += buf;
        }
    }
    return encoded;
}
