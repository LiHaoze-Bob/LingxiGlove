#ifndef HTTP_CLIENT_H
#define HTTP_CLIENT_H

#include <WiFi.h>
#include <HTTPClient.h>
#include <functional>

// 发送HTTP POST请求（JSON格式）
// 返回响应正文字符串，失败返回空字符串
String httpPostJson(const char* url, const String& jsonPayload, const char* authHeader = nullptr);

/**
 * @brief 以 SSE（Server-Sent Events）流式方式发送 HTTP POST 请求。
 *
 * 适用于 DashScope "X-DashScope-SSE: enable" 模式：服务端以
 *   data: <json>\n\n
 * 的格式逐行推送事件，客户端边收边处理，比等待完整响应更低延迟。
 *
 * 实现上使用 HTTPClient::getStreamPtr() 逐字节读取，以 '\n' 为分隔符
 * 拼出完整行后交给 onLine 回调。回调返回 true 继续读取，返回 false 立即
 * 中止（调用方据此实现"拿到足够结果就提前终止"逻辑）。
 *
 * @param url         请求 URL（支持 HTTP / HTTPS）
 * @param jsonPayload POST 请求体（JSON 字符串）
 * @param authHeader  可选鉴权头（如 "Bearer sk-xxx"），nullptr 表示不加
 * @param onLine      每读到一行（去掉末尾 \r\n 的）文本调用一次；
 *                    返回 false 则提前终止读取
 * @return 服务端返回的 HTTP 状态码；< 0 表示连接失败
 */
int httpPostJsonSse(const char* url,
                    const String& jsonPayload,
                    const char* authHeader,
                    std::function<bool(const String& line)> onLine);

// 发送HTTP GET请求
// 返回响应正文字符串，失败返回空字符串
String httpGet(const char* url, const char* authHeader = nullptr);

// URL编码（用于TTS等需要编码中文的场景）
String urlEncode(const char* str);

#endif // HTTP_CLIENT_H
