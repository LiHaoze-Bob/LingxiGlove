// ============================================================
// test_esp_now_sync.cpp
// HandFrame 线上布局的 host-side 契约测试
// ------------------------------------------------------------
// 目的：
//   HandFrame 会在 MASTER / SLAVE 两颗 MCU 之间按字节拷贝传输，
//   一旦 packed / 字段顺序 / 类型宽度被误改，就会出现 "本机能跑、
//   对端解错" 的幽灵问题。这里用 offsetof + sizeof 固定住契约，
//   任何破坏性改动都会让本测试 FAIL，迫使作者同步更新对端。
//
// 说明：
//   - 本测试直接 include 项目的 esp_now_sync.h（通过 ../../LingxiGlove_Main/
//     相对路径）。esp_now_sync.h 自身 include "config.h"，GCC 会在该 header
//     所在目录解析嵌套 include，从而共享项目真实 config.h（单一真相源）。
//     本测试目录故意不放 config.h stub，避免出现"两份 config"漂移。
//   - 仅验证 POD 契约，不调用任何 ESP-NOW API（host 上无实现）。
//
// 编译运行：
//   cd src/tests/test_esp_now_sync
//   make run
// ============================================================

#include <cstdio>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <type_traits>

// 直接拿项目 header 当被测对象；ENABLE_ESPNOW_SYNC 不影响 HandFrame 定义
#include "../../LingxiGlove_Main/esp_now_sync.h"

static int g_pass = 0;
static int g_fail = 0;
#define EXPECT(cond, msg) do {                                   \
    if (cond) { g_pass++; }                                      \
    else {                                                       \
        g_fail++;                                                \
        std::fprintf(stderr, "[FAIL] %s:%d  %s  (cond: %s)\n",   \
                     __FILE__, __LINE__, msg, #cond);            \
    }                                                            \
} while (0)

int main() {
    std::printf("========================================\n");
    std::printf(" esp_now_sync HandFrame layout contract\n");
    std::printf("========================================\n");

    // ---- FLEX_CHANNEL_COUNT 必须与 sensor_manager 一致（配置单一来源） ----
    EXPECT(FLEX_CHANNEL_COUNT == 5, "FLEX_CHANNEL_COUNT is 5 (MVP 默认)");

    // ---- POD 性质：简单可复制 / 标准布局 ----
    EXPECT(std::is_trivially_copyable<HandFrame>::value,
           "HandFrame must be trivially copyable (按字节拷贝 safe)");
    EXPECT(std::is_standard_layout<HandFrame>::value,
           "HandFrame must be standard layout (跨编译器一致)");

    // ---- sizeof 严格等于各字段字节之和（packed 证据） ----
    constexpr size_t expected_size =
        4  /*master_timestamp_ms*/ +
        2  /*seq_no*/ +
        1  /*frame_type*/ +
        1  /*proto_version*/ +
        2 * 6  /*ax..gz*/ +
        2 * FLEX_CHANNEL_COUNT /*flex[]*/;
    EXPECT(sizeof(HandFrame) == expected_size,
           "sizeof(HandFrame) 与字段字节和严格相等，无 padding");

    // ---- 逐字段 offset 固定契约 ----
    EXPECT(offsetof(HandFrame, master_timestamp_ms) == 0,
           "master_timestamp_ms @ offset 0");
    EXPECT(offsetof(HandFrame, seq_no)              == 4,
           "seq_no @ offset 4");
    EXPECT(offsetof(HandFrame, frame_type)           == 6,
           "frame_type @ offset 6");
    EXPECT(offsetof(HandFrame, proto_version)        == 7,
           "proto_version @ offset 7");
    EXPECT(offsetof(HandFrame, ax)                  == 8,
           "ax @ offset 8");
    EXPECT(offsetof(HandFrame, ay)                  == 10, "ay @ offset 10");
    EXPECT(offsetof(HandFrame, az)                  == 12, "az @ offset 12");
    EXPECT(offsetof(HandFrame, gx)                  == 14, "gx @ offset 14");
    EXPECT(offsetof(HandFrame, gy)                  == 16, "gy @ offset 16");
    EXPECT(offsetof(HandFrame, gz)                  == 18, "gz @ offset 18");
    EXPECT(offsetof(HandFrame, flex)                == 20, "flex[] @ offset 20");

    // ---- 字段宽度校验（防止未来有人把 int16_t 改成 int32_t） ----
    EXPECT(sizeof(((HandFrame*)0)->master_timestamp_ms) == 4,
           "master_timestamp_ms is 32-bit");
    EXPECT(sizeof(((HandFrame*)0)->seq_no)              == 2,
           "seq_no is 16-bit");
    EXPECT(sizeof(((HandFrame*)0)->ax)                  == 2,
           "ax is int16_t");
    EXPECT(sizeof(((HandFrame*)0)->flex[0])             == 2,
           "flex[i] is uint16_t");

    // ---- 序列化 / 反序列化等价：打一个样本值再按字节恢复 ----
    HandFrame sent = {};
    sent.master_timestamp_ms = 0x11223344u;
    sent.seq_no              = 0xABCD;
    sent.frame_type          = FRAME_TYPE_SENSOR_DATA;
    sent.proto_version       = HANDFRAME_PROTO_VERSION;
    sent.ax = 1000; sent.ay = -2000; sent.az = 16000;
    sent.gx = -3; sent.gy = 4; sent.gz = -5;
    for (size_t i = 0; i < FLEX_CHANNEL_COUNT; i++) {
        sent.flex[i] = (uint16_t)(100 * (i + 1));
    }

    uint8_t wire[sizeof(HandFrame)];
    std::memcpy(wire, &sent, sizeof(HandFrame));

    HandFrame recv;
    std::memcpy(&recv, wire, sizeof(HandFrame));

    EXPECT(recv.master_timestamp_ms == sent.master_timestamp_ms,
           "timestamp round-trip");
    EXPECT(recv.seq_no == sent.seq_no, "seq_no round-trip");
    EXPECT(recv.ax == sent.ax && recv.ay == sent.ay && recv.az == sent.az,
           "accel round-trip");
    EXPECT(recv.gx == sent.gx && recv.gy == sent.gy && recv.gz == sent.gz,
           "gyro round-trip");
    bool flex_ok = true;
    for (size_t i = 0; i < FLEX_CHANNEL_COUNT; i++) {
        if (recv.flex[i] != sent.flex[i]) { flex_ok = false; break; }
    }
    EXPECT(flex_ok, "flex[] round-trip");

    // ---- ESP-NOW 单包载荷上限 250 bytes；HandFrame 必须远小于此 ----
    EXPECT(sizeof(HandFrame) <= 250,
           "sizeof(HandFrame) <= 250 (ESP-NOW max payload)");

    std::printf("----------------------------------------\n");
    std::printf(" PASSED: %d   FAILED: %d\n", g_pass, g_fail);
    std::printf("----------------------------------------\n");
    return g_fail == 0 ? 0 : 1;
}
