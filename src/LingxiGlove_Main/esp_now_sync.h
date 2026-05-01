// ============================================================
// esp_now_sync.h
// 双手手语同步通信接口（ESP-NOW）
// ------------------------------------------------------------
// 职责：
//   为"双手手语翻译"方案提供一条短延迟、无 AP 依赖的 MCU-MCU 通道，
//   在两只手套（MASTER / SLAVE）之间以固定采样率交换同步的 HandFrame。
//
// 非职责：
//   - 不做时钟对齐 / 卡尔曼融合（由上层模块处理，使用 HandFrame 里的
//     master_timestamp_ms 做时间对齐）
//   - 不做 WiFi AP 模式切换（使用方须知：ESP-NOW 与普通 Wi-Fi 共用
//     射频，必须同信道，本模块不强制设定信道）
//
// 约束：
//   - 本头文件**不 include <esp_now.h>**，只暴露 POD 与函数签名，
//     让 ENABLE_ESPNOW_SYNC=0 的编译路径完全不引入 ESP-NOW 依赖
//   - 接口为纯 C++11，与主程序解耦；.cpp 中根据宏决定是否真的打开
//     ESP-NOW 硬件
// ============================================================

#ifndef ESP_NOW_SYNC_H
#define ESP_NOW_SYNC_H

#include <stdint.h>
#include <stddef.h>

#include "config.h"   // FLEX_CHANNEL_COUNT, ENABLE_ESPNOW_SYNC

/**
 * @brief 一帧双手同步数据的线上格式 (wire format)
 *
 * 设计要点：
 *   - __attribute__((packed)) 确保 MASTER / SLAVE 两侧 MCU 使用相同布局
 *     （ESP32-S3 与任何未来可能换的 MCU 之间都必须按字节一致）
 *   - 仅携带 MASTER 主时间戳 + 单调递增 seq_no，方便上层检测丢包 / 对齐
 *   - int16 原始 IMU 采样值 + uint16 原始 Flex ADC 值：节省空间，同时
 *     避免在发送端做浮点换算（由接收端统一换算，减少两侧口径不一致）
 *   - sizeof 完全由字段决定，host 单测会用 offsetof 固定每个字段位置
 */
struct __attribute__((packed)) HandFrame {
    uint32_t master_timestamp_ms;       // MASTER 主时钟毫秒戳（SLAVE 上报时回填 0，由 MASTER 打戳）
    uint16_t seq_no;                    // 0..65535 循环递增（按 SLAVE 侧采样步进）
    uint16_t reserved0;                 // 预留对齐（当前永远为 0）
    int16_t  ax;                        // 加速度原始值 (LSB, ±2g full-scale → 1LSB ≈ 0.000061 g)
    int16_t  ay;
    int16_t  az;
    int16_t  gx;                        // 陀螺原始值 (LSB, ±250°/s full-scale → 1LSB ≈ 0.00763 °/s)
    int16_t  gy;
    int16_t  gz;
    uint16_t flex[FLEX_CHANNEL_COUNT];  // 5 路弯曲传感器原始 ADC（0..4095）；未接线时发 0
};

/** 发送角色 */
enum EspNowRole {
    ESPNOW_ROLE_MASTER = 0,   // 主手：接收 SLAVE 上报，统一打时间戳
    ESPNOW_ROLE_SLAVE  = 1,   // 从手：采样后发帧给 MASTER
};

/**
 * @brief 收到合法 HandFrame 时触发的回调
 * @param frame   解包后（字节长度已校验）的帧引用
 * @param mac     发送方 MAC 地址，6 字节；仅在回调期间有效
 */
typedef void (*HandFrameHandler)(const HandFrame& frame, const uint8_t mac[6]);

/**
 * @brief 初始化 ESP-NOW 同步通道
 *
 * - ENABLE_ESPNOW_SYNC=0 时恒返回 false，不触碰任何硬件
 * - ENABLE_ESPNOW_SYNC=1 时：
 *     * 设置 WiFi 为 STA 模式但不连 AP
 *     * 初始化 ESP-NOW，注册发送/接收回调
 *     * 若 role=MASTER 且 peer_mac 非 nullptr，则注册该 SLAVE 为 peer
 *     * 若 role=SLAVE  且 peer_mac 非 nullptr，则注册该 MASTER 为 peer；
 *       若 peer_mac 为 nullptr 则注册 FF:FF:FF:FF:FF:FF 广播 peer
 *
 * @param role      本机角色
 * @param peer_mac  对端 MAC，6 字节；可为 nullptr（由 SLAVE 走广播时）
 * @return 成功 true；参数非法 / 编译期禁用 / ESP-NOW 初始化失败 → false
 */
bool InitEspNowSync(EspNowRole role, const uint8_t peer_mac[6]);

/**
 * @brief 发送一帧 HandFrame 给对端
 *
 * - ENABLE_ESPNOW_SYNC=0 时恒返回 false
 * - 发送是异步的：成功入队返回 true，真正送达与否通过 send_cb 的日志反馈
 *
 * @param frame 要发送的帧（按值传入的副本更稳妥，调用方不必维持其生命周期）
 * @return 入队成功 true；否则 false
 */
bool SendHandFrame(const HandFrame& frame);

/**
 * @brief 注册接收回调；可多次调用，后者覆盖前者（只保留 1 个 handler）
 *
 * - ENABLE_ESPNOW_SYNC=0 时为空操作
 * - 传 nullptr 等价于取消注册
 */
void RegisterHandFrameHandler(HandFrameHandler handler);

/**
 * @brief 反初始化；释放 ESP-NOW 占用。ENABLE_ESPNOW_SYNC=0 时为空操作
 */
void ShutdownEspNowSync();

/**
 * @brief 返回自 Init 以来收到的有效 HandFrame 数量（调试 / 自检用）
 *        - 有效 = 长度正好等于 sizeof(HandFrame) 的包
 *        - ENABLE_ESPNOW_SYNC=0 时恒返回 0
 *        - ShutdownEspNowSync 会清零本计数
 */
uint32_t GetEspNowRxCount();

/**
 * @brief 返回自 Init 以来 send_cb 报告 SUCCESS 的次数
 *        - 注意：这是"ACK 到端"的次数，不是"入队"次数；
 *          入队但未 ACK 的包不计入此值，会计入 GetEspNowTxFailCount()
 *        - ENABLE_ESPNOW_SYNC=0 时恒返回 0
 *        - ShutdownEspNowSync 会清零本计数
 */
uint32_t GetEspNowTxCount();

/**
 * @brief 返回自 Init 以来 send_cb 报告 FAIL 的次数
 *        - 用途：与 GetEspNowTxCount() 合计可算得发送成功率
 *        - ENABLE_ESPNOW_SYNC=0 时恒返回 0
 *        - ShutdownEspNowSync 会清零本计数
 */
uint32_t GetEspNowTxFailCount();

#endif  // ESP_NOW_SYNC_H
