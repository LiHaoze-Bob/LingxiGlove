#ifndef HTTP_CLIENT_H
#define HTTP_CLIENT_H

#include <WiFi.h>
#include <HTTPClient.h>
#include <functional>

/**
 * @brief 发送 HTTP POST 请求（JSON 格式），返回响应正文字符串。
 *
 * @param url         请求 URL（支持 HTTP / HTTPS）
 * @param jsonPayload POST 请求体（JSON 字符串）
 * @param authHeader  可选鉴权头（如 "Bearer sk-xxx"），nullptr 表示不加
 * @param timeout_ms  可选超时（毫秒），0 表示使用默认值（15s）
 * @return 响应正文字符串，失败返回空字符串
 */
String httpPostJson(const char* url, const String& jsonPayload,
                    const char* authHeader = nullptr, uint16_t timeout_ms = 0);

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

/**
 * @brief GET 请求下载二进制数据到指定缓冲区（适用于 WAV 等二进制文件下载）。
 *
 * 与 httpGet() 返回 String 不同，本函数直接将响应体写入 caller 提供的
 * uint8_t 缓冲区，避免 String 拷贝开销，适合下载 TTS 音频等大二进制文件。
 *
 * @param url         GET 请求 URL（支持 HTTP / HTTPS）
 * @param dst         目标缓冲区指针（caller 负责分配，通常为 PSRAM）
 * @param dst_size    缓冲区最大字节数
 * @param authHeader  可选鉴权头，nullptr 表示不加
 * @return 实际下载的字节数；0 表示失败
 */
size_t httpGetToBuffer(const char* url, uint8_t* dst, size_t dst_size,
                       const char* authHeader = nullptr);

// URL编码（用于TTS等需要编码中文的场景）
String urlEncode(const char* str);

#endif // HTTP_CLIENT_H
