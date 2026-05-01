#ifndef WIFI_MANAGER_H
#define WIFI_MANAGER_H

#include <WiFi.h>

// 连接WiFi（阻塞式，带超时）
bool connectWiFi(const char* ssid, const char* password, unsigned long timeoutMs = 10000);

// 检查WiFi是否已连接
bool isWiFiConnected();

// 非阻塞式WiFi状态检查，断线时自动重连
void checkWiFiConnection(const char* ssid, const char* password);

// 打印WiFi连接信息
void printWiFiInfo();

#endif // WIFI_MANAGER_H
