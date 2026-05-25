// ============================================================
// nvs_config.cpp
// ESP-NOW 角色与对端 MAC 的 NVS 持久化配置实现
// ============================================================

#include "nvs_config.h"
#include "config.h"

#include <Arduino.h>
#include <Preferences.h>
#include <stdio.h>
#include <string.h>

// ------------------- NVS 常量 -------------------
static const char* kNvsNamespace = "espnow_cfg";
static const char* kKeyRole      = "role";
static const char* kKeyPeerMac   = "peer_mac";

// ------------------- 角色读写 -------------------

bool LoadNvsRole(uint8_t* out_role) {
    if (!out_role) return false;

    Preferences prefs;
    if (!prefs.begin(kNvsNamespace, true)) {
        return false;
    }

    // getUChar 第二个参数是默认值；用 0xFF 做哨兵，区分"有值"和"无值"
    uint8_t value = prefs.getUChar(kKeyRole, 0xFF);
    prefs.end();

    if (value == 0xFF) {
        return false;  // NVS 中无记录
    }
    *out_role = value;
    return true;
}

bool SaveNvsRole(uint8_t role) {
    if (role > 1) return false;  // 仅允许 0(MASTER) 或 1(SLAVE)

    Preferences prefs;
    if (!prefs.begin(kNvsNamespace, false)) {
        return false;
    }

    size_t written = prefs.putUChar(kKeyRole, role);
    prefs.end();
    return (written == 1);
}

// ------------------- 对端 MAC 读写 -------------------

bool LoadNvsPeerMac(uint8_t out_mac[6]) {
    if (!out_mac) return false;

    Preferences prefs;
    if (!prefs.begin(kNvsNamespace, true)) {
        return false;
    }

    size_t read_len = prefs.getBytes(kKeyPeerMac, out_mac, 6);
    prefs.end();
    return (read_len == 6);
}

bool SaveNvsPeerMac(const uint8_t mac[6]) {
    if (!mac) return false;

    Preferences prefs;
    if (!prefs.begin(kNvsNamespace, false)) {
        return false;
    }

    size_t written = prefs.putBytes(kKeyPeerMac, mac, 6);
    prefs.end();
    return (written == 6);
}

// ------------------- 清除配置 -------------------

bool ClearNvsConfig() {
    Preferences prefs;
    if (!prefs.begin(kNvsNamespace, false)) {
        return false;
    }
    bool ok = prefs.clear();
    prefs.end();
    return ok;
}

// ------------------- MAC 字符串解析 -------------------

bool ParseMacString(const char* str, uint8_t out_mac[6]) {
    if (!str || !out_mac) return false;

    unsigned int values[6];
    int parsed = sscanf(str, "%x:%x:%x:%x:%x:%x",
                        &values[0], &values[1], &values[2],
                        &values[3], &values[4], &values[5]);
    if (parsed != 6) return false;

    for (int i = 0; i < 6; i++) {
        if (values[i] > 0xFF) return false;
        out_mac[i] = (uint8_t)values[i];
    }
    return true;
}

void FormatMacString(const uint8_t mac[6], char* out_str, size_t buf_size) {
    if (!mac || !out_str || buf_size < 18) return;
    snprintf(out_str, buf_size, "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// ------------------- WiFi 凭据读写 -------------------

static const char* kNvsWifiNamespace = "wifi_cfg";
static const char* kKeyWifiSsid     = "ssid";
static const char* kKeyWifiPass     = "pass";

bool LoadNvsWifiSsid(char* out_ssid, size_t buf_size) {
    if (!out_ssid || buf_size < 2) return false;

    Preferences prefs;
    if (!prefs.begin(kNvsWifiNamespace, true)) {
        return false;
    }

    String value = prefs.getString(kKeyWifiSsid, "");
    prefs.end();

    if (value.length() == 0) return false;
    if (value.length() + 1 > buf_size) return false;

    strncpy(out_ssid, value.c_str(), buf_size);
    out_ssid[buf_size - 1] = '\0';
    return true;
}

bool LoadNvsWifiPassword(char* out_pass, size_t buf_size) {
    if (!out_pass || buf_size < 2) return false;

    Preferences prefs;
    if (!prefs.begin(kNvsWifiNamespace, true)) {
        return false;
    }

    String value = prefs.getString(kKeyWifiPass, "");
    prefs.end();

    if (value.length() == 0) return false;
    if (value.length() + 1 > buf_size) return false;

    strncpy(out_pass, value.c_str(), buf_size);
    out_pass[buf_size - 1] = '\0';
    return true;
}

bool SaveNvsWifi(const char* ssid, const char* password) {
    if (!ssid || strlen(ssid) == 0) return false;
    if (!password) return false;  // 允许空密码（开放网络）

    Preferences prefs;
    if (!prefs.begin(kNvsWifiNamespace, false)) {
        return false;
    }

    size_t s1 = prefs.putString(kKeyWifiSsid, ssid);
    size_t s2 = prefs.putString(kKeyWifiPass, password);
    prefs.end();
    return (s1 > 0 && s2 > 0);
}

bool ClearNvsWifi() {
    Preferences prefs;
    if (!prefs.begin(kNvsWifiNamespace, false)) {
        return false;
    }
    bool ok = prefs.clear();
    prefs.end();
    return ok;
}
