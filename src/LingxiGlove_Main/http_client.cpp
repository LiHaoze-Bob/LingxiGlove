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
    // WiFiClientSecure::setTimeout 单位为秒，控制 TLS 握手阶段的 TCP socket 超时。
    // TLS 握手典型耗时 1-2s，设 5s 足够，不与 http.setTimeout（读取层）叠加到 30s。
    secure_client.setTimeout(5);
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
        DEBUG_LOG("[HTTP] begin 失败: %s", url);
        return String();
    }

    http.setTimeout(kHttpTimeoutMs);
    http.setConnectTimeout(kHttpTimeoutMs);
    http.addHeader("Content-Type", "application/json");

    if (authHeader != nullptr) {
        http.addHeader("Authorization", authHeader);
    }

    DEBUG_LOG("[HTTP] POST %s", url);

    int httpCode = http.POST(jsonPayload);
    String response;

    if (httpCode > 0) {
        DEBUG_LOG("[HTTP] 状态码: %d", httpCode);
        response = http.getString();
    } else {
        DEBUG_LOG("[HTTP] 请求失败, 错误: %s", http.errorToString(httpCode).c_str());
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
        DEBUG_LOG("[HTTP-SSE] begin 失败: %s", url);
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

    DEBUG_LOG("[HTTP-SSE] POST %s", url);

    int http_code = http.POST(jsonPayload);
    if (http_code <= 0) {
        DEBUG_LOG("[HTTP-SSE] 请求失败: %s", http.errorToString(http_code).c_str());
        http.end();
        return http_code;
    }

    DEBUG_LOG("[HTTP-SSE] 状态码: %d", http_code);

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
            DEBUG_LOG("[HTTP-SSE] 错误体: %s", err_buf);
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

    // 批量读取 SSE 流，以 '\n' 为行分隔符，组装完整行后交给 onLine 回调。
    //
    // 旧方案（逐字节）的问题：
    //   每帧 base64 数据约 8000 字节，逐字节 read() 需要循环 8000 次；
    //   每次 available()==0 就 delay(2)，导致实际读取速度比 WiFi 吞吐慢 10 倍以上。
    //   实测：理论播放 0.76s 的 TTS 音频，实际耗时 9s（卡顿明显）。
    //
    // 新方案（批量读取）：
    //   每轮循环一次性读入所有 available 字节到 s_batch_buf，
    //   再用 memchr 扫 '\n' 分割行，消除了逐字节 delay 的瓶颈。
    //   s_batch_buf 放 .bss 段（static），避免栈溢出。
    // s_line_buf 必须能容纳单帧 SSE 数据的完整一行：
    //   实测 Qwen-TTS 单帧 base64 最大约 16350 B + JSON 包装约 80 B = ~16430 B，
    //   16384 B 不够用（实测 truncate=3，max_line=16383 是被截后的长度）。
    //   扩到 24576 B（24KB）留 ~50% 余量，单次扩容一次性解决。
    static char s_line_buf[24576]; // 当前行的组装缓冲（最大单行长度）
    static char s_batch_buf[2048]; // 每次 readBytes 的批量接收缓冲
    int line_len = 0;
    const unsigned long kReadTimeout = 20000UL;  // 单次流读取总超时
    unsigned long deadline = millis() + kReadTimeout;
    bool stop_early = false;

    // [DIAG] 临时诊断计数器：行长统计 + 截断次数
    int diag_total_lines       = 0;
    int diag_data_lines        = 0;
    int diag_max_line_len      = 0;
    int diag_truncate_count    = 0;
    int diag_first_data_len    = -1;

    while (!stop_early && millis() < deadline) {
        int avail = stream->available();
        if (avail <= 0) {
            if (!http.connected()) break;
            // 无数据时短暂让出 CPU，但不 delay(2)（那会把每字节都拖慢 2ms）
            delay(1);
            continue;
        }

        // 更新 deadline：只要还在收数据就续期
        deadline = millis() + kReadTimeout;

        // 批量读入，最多读满 s_batch_buf
        int to_read = avail < (int)sizeof(s_batch_buf) ? avail : (int)sizeof(s_batch_buf);
        int got = (int)stream->readBytes(s_batch_buf, to_read);
        if (got <= 0) continue;

        // 扫描本批数据，按 '\n' 切行
        const char* p = s_batch_buf;
        const char* end = s_batch_buf + got;
        while (p < end && !stop_early) {
            // 找下一个 '\n'
            const char* nl = (const char*)memchr(p, '\n', (size_t)(end - p));
            if (nl == nullptr) {
                // 本批没有换行符，全部追加到 line 缓冲
                int chunk = (int)(end - p);
                int space = (int)sizeof(s_line_buf) - 1 - line_len;
                if (chunk > space) {
                    diag_truncate_count++;  // [DIAG] 行被截断，行内 base64 必坏
                    chunk = space;
                }
                if (chunk > 0) {
                    memcpy(s_line_buf + line_len, p, chunk);
                    line_len += chunk;
                }
                break;
            }

            // 把 [p, nl) 追加到 line 缓冲（nl 指向 '\n'，不含）
            int chunk = (int)(nl - p);
            int space = (int)sizeof(s_line_buf) - 1 - line_len;
            if (chunk > space) {
                diag_truncate_count++;  // [DIAG] 行被截断
                chunk = space;
            }
            if (chunk > 0) {
                memcpy(s_line_buf + line_len, p, chunk);
                line_len += chunk;
            }

            // 去掉末尾的 '\r'（Windows 风格 \r\n）
            if (line_len > 0 && s_line_buf[line_len - 1] == '\r') {
                --line_len;
            }
            s_line_buf[line_len] = '\0';

            if (line_len > 0) {
                // [DIAG] 行长统计
                diag_total_lines++;
                if (line_len > diag_max_line_len) diag_max_line_len = line_len;
                if (line_len > 5 && memcmp(s_line_buf, "data:", 5) == 0) {
                    diag_data_lines++;
                    if (diag_first_data_len < 0) diag_first_data_len = line_len;
                }

                String line_str(s_line_buf);
                if (!onLine(line_str)) {
                    stop_early = true;
                }
            }

            // 移动到 '\n' 之后继续扫
            p = nl + 1;
            line_len = 0;
        }
    }

    // [DIAG] 行解析诊断汇总：能看出 base64 是否被 s_line_buf 截断
    DEBUG_LOG("[HTTP-SSE-DIAG] lines=%d data_lines=%d max_line=%d first_data=%d truncate=%d (buf=%d)",
              diag_total_lines, diag_data_lines, diag_max_line_len,
              diag_first_data_len, diag_truncate_count, (int)sizeof(s_line_buf));

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
        DEBUG_LOG("[HTTP] begin 失败: %s", url);
        return String();
    }

    http.setTimeout(kHttpTimeoutMs);
    http.setConnectTimeout(kHttpTimeoutMs);

    if (authHeader != nullptr) {
        http.addHeader("Authorization", authHeader);
    }

    DEBUG_LOG("[HTTP] GET %s", url);

    int httpCode = http.GET();
    String response;

    if (httpCode > 0) {
        DEBUG_LOG("[HTTP] 状态码: %d", httpCode);
        response = http.getString();
    } else {
        DEBUG_LOG("[HTTP] 请求失败, 错误: %s", http.errorToString(httpCode).c_str());
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
