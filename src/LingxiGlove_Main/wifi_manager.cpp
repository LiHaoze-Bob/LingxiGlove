#include "wifi_manager.h"
#include "config.h"

bool connectWiFi(const char* ssid, const char* password, unsigned long timeoutMs) {
    DEBUG_PRINTLN("[WiFi] 开始连接WiFi...");
    DEBUG_PRINT("[WiFi] SSID: ");
    DEBUG_PRINTLN(ssid);

    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);

    unsigned long startTime = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - startTime > timeoutMs) {
            DEBUG_PRINTLN("[WiFi] 连接超时！");
            return false;
        }
        delay(500);
        DEBUG_PRINT(".");
    }

    DEBUG_PRINTLN();
    DEBUG_PRINTLN("[WiFi] 连接成功！");
    printWiFiInfo();
    return true;
}

bool isWiFiConnected() {
    return WiFi.status() == WL_CONNECTED;
}

void checkWiFiConnection(const char* ssid, const char* password) {
    static unsigned long lastCheck = 0;
    if (millis() - lastCheck < 5000) return;  // 每5秒检查一次
    lastCheck = millis();

    if (!isWiFiConnected()) {
        DEBUG_PRINTLN("[WiFi] 连接断开，尝试重连...");
        connectWiFi(ssid, password, WIFI_TIMEOUT_MS);
    }
}

void printWiFiInfo() {
    DEBUG_PRINT("[WiFi] IP地址: ");
    DEBUG_PRINTLN(WiFi.localIP());
    DEBUG_PRINT("[WiFi] 信号强度: ");
    DEBUG_PRINT(WiFi.RSSI());
    DEBUG_PRINTLN(" dBm");
}
