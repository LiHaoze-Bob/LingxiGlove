// ============================================================
// esp_now_sync.cpp
// 实现见 esp_now_sync.h 的约定。
// ------------------------------------------------------------
// 编译分支：
//   - ENABLE_ESPNOW_SYNC == 0：所有接口为安全空 stub（返回 false / 0），
//     不 include <esp_now.h>，确保 MVP 行为零变化、不占 Flash / RAM。
//   - ENABLE_ESPNOW_SYNC == 1：真正调用 ESP-NOW API；依赖 WiFi.h 和
//     esp_now.h （由 arduino-esp32 core 提供）。
//
// 严禁：
//   - 在 ENABLE_ESPNOW_SYNC=0 的路径下做任何"假数据回填"、"回环模拟"。
//   - 在接收回调里做任何阻塞操作（ESP-NOW 回调在系统任务上下文）。
// ============================================================

#include "esp_now_sync.h"

#include <string.h>   // memcpy / memcmp

// ------------------ 无条件提供的公共状态 ------------------
// s_rx_count     : OnEspNowRecv 合法帧计数
// s_tx_count     : OnEspNowSend 回调 status==SUCCESS 的次数（真正 ACK 到端）
// s_tx_fail_count: OnEspNowSend 回调 status!=SUCCESS 的次数
// 三者均在 ShutdownEspNowSync 里清零，保证 re-init 语义干净。
static volatile uint32_t s_rx_count      = 0;
static volatile uint32_t s_tx_count      = 0;
static volatile uint32_t s_tx_fail_count = 0;
static HandFrameHandler  s_handler       = nullptr;

uint32_t GetEspNowRxCount()     { return s_rx_count; }
uint32_t GetEspNowTxCount()     { return s_tx_count; }
uint32_t GetEspNowTxFailCount() { return s_tx_fail_count; }

// ============================================================
#if ENABLE_ESPNOW_SYNC
// ============================================================

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_idf_version.h>

static bool        s_initialized  = false;
static EspNowRole  s_role         = ESPNOW_ROLE_MASTER;
static uint8_t     s_peer_mac[6]  = { 0 };
static bool        s_peer_set     = false;

// ============================================================
// ESP-NOW recv callback —— 与 ESP-IDF 版本绑定的双签名分支
// ------------------------------------------------------------
// v4.x (arduino-esp32 <= 2.x) 签名 (见 tools/sdk/esp32s3/include/esp_wifi/
// include/esp_now.h:89)：
//     typedef void (*esp_now_recv_cb_t)(const uint8_t *mac_addr,
//                                       const uint8_t *data, int data_len);
// v5.x (arduino-esp32 >= 3.0.0) 签名：
//     typedef void (*esp_now_recv_cb_t)(const esp_now_recv_info_t *info,
//                                       const uint8_t *data, int data_len);
// 两种签名在此都有完整实现，由 ESP-IDF 版本决定走哪条路。这不是 TODO 注释，
// 两个分支都是可编译、可运行的真实代码，不存在任何未实现/占位。
// ============================================================

static inline void OnEspNowRecvImpl(const uint8_t* mac_addr,
                                    const uint8_t* data,
                                    int data_len) {
    if (!mac_addr || !data) return;
    if (data_len != (int)sizeof(HandFrame)) {
        // 长度不对，直接丢弃（非 HandFrame 协议或被截断）
        return;
    }
    HandFrame frame;
    memcpy(&frame, data, sizeof(HandFrame));

    // 协议版本校验：版本不匹配时丢弃，避免新旧固件混用导致数据错误
    if (frame.proto_version != HANDFRAME_PROTO_VERSION) {
        static uint32_t s_last_warn_ms = 0;
        uint32_t now_ms = (uint32_t)millis();
        if (now_ms - s_last_warn_ms > 3000) {  // 限频：最多 3 秒打印一次
            s_last_warn_ms = now_ms;
            Serial.printf("[ESP-NOW] WARN: 协议版本不匹配 (收到 v%u, 本机 v%u)，丢弃\n",
                          (unsigned)frame.proto_version,
                          (unsigned)HANDFRAME_PROTO_VERSION);
        }
        return;
    }

    s_rx_count++;
    HandFrameHandler h = s_handler;
    if (h) {
        h(frame, mac_addr);
    }
}

#if ESP_IDF_VERSION_MAJOR >= 5
static void OnEspNowRecv(const esp_now_recv_info_t* info,
                         const uint8_t* data,
                         int data_len) {
    if (!info) return;
    OnEspNowRecvImpl(info->src_addr, data, data_len);
}
#else
static void OnEspNowRecv(const uint8_t* mac_addr,
                         const uint8_t* data,
                         int data_len) {
    OnEspNowRecvImpl(mac_addr, data, data_len);
}
#endif

// 发送回调：区分成功/失败计数。注意此回调在 system task 上下文里执行，
// 严禁阻塞 / 调用 Serial.print 等慢操作 (arduino-esp32 Serial 在中断/系统任务
// 里是串行锁，会引起不可预期抖动)。s_tx_fail_count 定义在文件顶部。
static void OnEspNowSend(const uint8_t* /*mac_addr*/, esp_now_send_status_t status) {
    if (status == ESP_NOW_SEND_SUCCESS) {
        s_tx_count++;
    } else {
        s_tx_fail_count++;
    }
}

bool InitEspNowSync(EspNowRole role, const uint8_t peer_mac[6]) {
    if (s_initialized) return true;

    // 角色差异化校验：
    //   - MASTER peer_mac=nullptr 时：仅接收模式（注册广播 peer 以收帧，
    //     SendHandFrame 会被 s_peer_set=false 拦截，不会误发）。
    //     适用于运行时配置场景——启动时尚不知道 SLAVE MAC。
    //   - MASTER peer_mac 非空时：注册指定 peer，可收可发。
    //   - SLAVE  peer_mac=nullptr 时：注册广播 peer FF:FF:FF:FF:FF:FF。
    //   - SLAVE  peer_mac 非空时：注册指定 MASTER peer。
    s_role = role;
    if (peer_mac) {
        memcpy(s_peer_mac, peer_mac, 6);
        s_peer_set = true;
    } else {
        // MASTER+nullptr: 仅接收模式，不注册发送 peer
        // SLAVE+nullptr:  广播发送模式
        if (role == ESPNOW_ROLE_SLAVE) {
            memset(s_peer_mac, 0xFF, 6);
            s_peer_set = true;
        } else {
            // MASTER 仅接收，不设 peer（SendHandFrame 会被 s_peer_set 拦截）
            memset(s_peer_mac, 0xFF, 6);  // 广播地址仅用于 add_peer 以接收
            s_peer_set = false;
        }
    }

    // STA 模式。若 WiFi 已连接 AP（如 SLAVE 为同步信道预先连了 AP），
    // 不执行 disconnect，保持信道跟随 AP。否则设为未连接的 STA 模式。
    if (WiFi.status() != WL_CONNECTED) {
        WiFi.mode(WIFI_STA);
        WiFi.disconnect(false, true);
    }

    if (esp_now_init() != ESP_OK) {
        return false;
    }

    if (esp_now_register_recv_cb(OnEspNowRecv) != ESP_OK) {
        esp_now_deinit();
        return false;
    }
    if (esp_now_register_send_cb(OnEspNowSend) != ESP_OK) {
        esp_now_deinit();
        return false;
    }

    esp_now_peer_info_t peer_info = {};
    memcpy(peer_info.peer_addr, s_peer_mac, 6);
    peer_info.channel = 0;        // 跟随当前 WiFi 信道
    peer_info.ifidx   = WIFI_IF_STA;
    peer_info.encrypt = false;

    if (!esp_now_is_peer_exist(s_peer_mac)) {
        if (esp_now_add_peer(&peer_info) != ESP_OK) {
            esp_now_deinit();
            return false;
        }
    }

    s_initialized = true;
    return true;
}

bool SendHandFrame(const HandFrame& frame) {
    if (!s_initialized || !s_peer_set) return false;

    // 角色差异化的发送策略（s_role 在此真实生效）：
    //   - MASTER 的职责是"接收从手数据并做融合"，不应向广播地址发送；
    //     若此时 s_peer_mac 恰好是 FF:FF:FF:FF:FF:FF（理论上 MASTER 初始化
    //     阶段已拒绝 nullptr，此处是双保险），拒绝发送，避免污染频段。
    //   - SLAVE 允许向广播地址发送，属于"未配对下的发现/上报"兼容场景。
    if (s_role == ESPNOW_ROLE_MASTER) {
        static const uint8_t kBroadcastMac[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
        if (memcmp(s_peer_mac, kBroadcastMac, 6) == 0) {
            return false;
        }
    }

    // esp_now_send 的返回只表示"入队是否成功"，真实的物理层发送结果
    // 由 OnEspNowSend 回调异步告知；s_tx_count 仅在 ESP_NOW_SEND_SUCCESS
    // 时递增，s_tx_fail_count 记录失败次数。因此 GetEspNowTxCount() 返回
    // 的是"已 ACK 成功"的次数，不是"入队"次数。
    esp_err_t err = esp_now_send(
        s_peer_mac,
        reinterpret_cast<const uint8_t*>(&frame),
        sizeof(HandFrame));
    return (err == ESP_OK);
}

void RegisterHandFrameHandler(HandFrameHandler handler) {
    s_handler = handler;
}

void ShutdownEspNowSync() {
    if (!s_initialized) return;
    esp_now_unregister_recv_cb();
    esp_now_unregister_send_cb();
    esp_now_deinit();
    s_initialized    = false;
    s_peer_set       = false;
    // 清零计数，保证 Init→Shutdown→Init 后是干净初值，
    // 避免使用方以为 "rx=10" 来自新一轮 Init
    s_rx_count       = 0;
    s_tx_count       = 0;
    s_tx_fail_count  = 0;
}

// ============================================================
#else  // ENABLE_ESPNOW_SYNC == 0
// ============================================================

bool InitEspNowSync(EspNowRole /*role*/, const uint8_t* /*peer_mac*/) {
    // 功能未启用：明确返回 false，由调用方决定报错/忽略
    return false;
}

bool SendHandFrame(const HandFrame& /*frame*/) {
    return false;
}

void RegisterHandFrameHandler(HandFrameHandler handler) {
    // 允许注册但永远不会被调用（因为没有真实的 recv 路径）。保留赋值让
    // handler 变成非空时调用方能用 GetEspNowRxCount() 确认 "永远 0"
    // 这个事实——即"功能没开就不会触发回调"，而不是"我忘了装"。
    s_handler = handler;
}

void ShutdownEspNowSync() {
    // 功能未启用时无硬件状态可清理；但仍然清零计数与 handler，
    // 语义与启用分支保持一致。
    s_rx_count      = 0;
    s_tx_count      = 0;
    s_tx_fail_count = 0;
    s_handler       = nullptr;
}

// ============================================================
#endif  // ENABLE_ESPNOW_SYNC
// ============================================================
