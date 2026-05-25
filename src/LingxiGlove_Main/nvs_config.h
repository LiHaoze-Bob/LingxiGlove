// ============================================================
// nvs_config.h
// ESP-NOW 角色与对端 MAC 的 NVS 持久化配置
// ------------------------------------------------------------
// 职责：
//   提供运行时角色（MASTER/SLAVE）和对端 MAC 地址的读写接口，
//   存储在 ESP32 NVS (Non-Volatile Storage) 中，掉电不丢失。
//
// 优先级：NVS > build_opt.h 编译期宏 > config.h 默认值
//   - NVS 中有值 → 使用 NVS 值
//   - NVS 中无值 → 回落到编译期 ESPNOW_ROLE 宏
//
// 串口交互：
//   role master   — 设为 MASTER 并重启
//   role slave    — 设为 SLAVE 并重启
//   peer AA:BB:CC:DD:EE:FF — 设置对端 MAC 并重启
//   info          — 打印当前角色、对端 MAC、本机 MAC
//   nvs clear     — 清除 NVS 配置（恢复编译期默认值）并重启
// ============================================================

#ifndef NVS_CONFIG_H
#define NVS_CONFIG_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/**
 * @brief 从 NVS 加载角色配置
 *
 * @param out_role 输出角色值：0=MASTER, 1=SLAVE
 * @return true 成功读取；false NVS 中无记录，调用方应使用编译期默认值
 */
bool LoadNvsRole(uint8_t* out_role);

/**
 * @brief 将角色写入 NVS
 *
 * @param role 0=MASTER, 1=SLAVE
 * @return true 写入成功
 */
bool SaveNvsRole(uint8_t role);

/**
 * @brief 从 NVS 加载对端 MAC 地址
 *
 * @param out_mac 输出 6 字节 MAC 地址
 * @return true 成功读取；false NVS 中无记录
 */
bool LoadNvsPeerMac(uint8_t out_mac[6]);

/**
 * @brief 将对端 MAC 地址写入 NVS
 *
 * @param mac 6 字节 MAC 地址
 * @return true 写入成功
 */
bool SaveNvsPeerMac(const uint8_t mac[6]);

/**
 * @brief 清除 NVS 中的角色和对端 MAC 配置（恢复编译期默认值）
 *
 * @return true 清除成功
 */
bool ClearNvsConfig();

// ------------------- WiFi 凭据 -------------------

/**
 * @brief 从 NVS 加载 WiFi SSID
 *
 * @param out_ssid 输出缓冲区
 * @param buf_size 缓冲区大小（建议 >= 33）
 * @return true 成功读取；false NVS 中无记录
 */
bool LoadNvsWifiSsid(char* out_ssid, size_t buf_size);

/**
 * @brief 从 NVS 加载 WiFi 密码
 *
 * @param out_pass 输出缓冲区
 * @param buf_size 缓冲区大小（建议 >= 65）
 * @return true 成功读取；false NVS 中无记录
 */
bool LoadNvsWifiPassword(char* out_pass, size_t buf_size);

/**
 * @brief 将 WiFi SSID 和密码写入 NVS
 *
 * @param ssid WiFi SSID（不超过 32 字符）
 * @param password WiFi 密码（不超过 64 字符）
 * @return true 写入成功
 */
bool SaveNvsWifi(const char* ssid, const char* password);

/**
 * @brief 清除 NVS 中的 WiFi 配置（恢复 secrets.h 编译期默认值）
 *
 * @return true 清除成功
 */
bool ClearNvsWifi();

/**
 * @brief 解析 "AA:BB:CC:DD:EE:FF" 格式的 MAC 字符串到 6 字节数组
 *
 * @param str 输入字符串（如 "AA:BB:CC:DD:EE:FF"）
 * @param out_mac 输出 6 字节数组
 * @return true 解析成功；false 格式非法
 */
bool ParseMacString(const char* str, uint8_t out_mac[6]);

/**
 * @brief 将 6 字节 MAC 格式化为 "AA:BB:CC:DD:EE:FF" 字符串
 *
 * @param mac 输入 6 字节 MAC
 * @param out_str 输出缓冲（至少 18 字节）
 * @param buf_size 缓冲大小
 */
void FormatMacString(const uint8_t mac[6], char* out_str, size_t buf_size);

#endif  // NVS_CONFIG_H
