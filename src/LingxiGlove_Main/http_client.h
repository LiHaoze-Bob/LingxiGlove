#ifndef HTTP_CLIENT_H
#define HTTP_CLIENT_H

#include <WiFi.h>
#include <HTTPClient.h>

// 发送HTTP POST请求（JSON格式）
// 返回响应正文字符串，失败返回空字符串
String httpPostJson(const char* url, const String& jsonPayload, const char* authHeader = nullptr);

// 发送HTTP GET请求
// 返回响应正文字符串，失败返回空字符串
String httpGet(const char* url, const char* authHeader = nullptr);

// URL编码（用于TTS等需要编码中文的场景）
String urlEncode(const char* str);

#endif // HTTP_CLIENT_H
