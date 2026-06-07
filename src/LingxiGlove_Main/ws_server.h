// ============================================================
// ws_server.h
// 端侧 WebSocket 服务器：与 LingxiGlove_APP 的 wsClient.ts/wsProto.ts 对齐。
//
// 帧 envelope（JSON 文本帧）:
//   { "v": 1, "kind": <kind>, "ts": <ms>, "payload": { ... } }
//
// 当前阶段实现的 kind：
//   - hello       双向握手；客户端连上后服务端先发 hello
//   - ping/pong   心跳保活
//   - mic_state   PTT 状态变更（state/level/recordingMs）
//   - audio_chunk 录音 PCM 块（base64 编码 16-bit LE 单声道）
//   - snapshot    周期性遥测快照（10Hz；录音期间降至 5Hz）
//                 节流由调用方（LingxiGlove_Main loop）负责，本模块只做组帧。
//   - tts_audio   TTS 流推送（演示模式专用）。客户端 hello.caps.demoMode=true
//                 才会订阅；payload 与 audio_chunk 同构 + 首帧带 text。
//                 由 tts_player 钩子在每次 i2s_write 边喂 DAC 边广播。
//
// 设计要点：
//   - 仅作 server，不主动外连；广播给所有已 hello 的客户端
//   - 与 mic_capture 解耦：本模块只接收 PCM int16 块 + 元数据，自行 base64
//   - ENABLE_WS_SERVER=0 时所有函数都是 no-op（保留链接符号）
// ============================================================
#ifndef WS_SERVER_H
#define WS_SERVER_H

#include <stdint.h>
#include <stddef.h>

#include "config.h"

#if !defined(ENABLE_WS_SERVER)
#define ENABLE_WS_SERVER 0
#endif

#ifdef __cplusplus
extern "C" {
#endif

// ---------- 数据结构 ----------

// snapshot 帧的输入。字段映射到 LingxiGlove_APP types.ts 的 SystemSnapshot：
//   .system    → 顶层 system 节点
//   .hands     → hands.left / hands.right
//   .mic       → mic 节点（spectrum 暂不实现，前端会得到空数组）
//   .gesture   → gesture 节点（仅最近一次仲裁的 top1，无候选历史）
// 注意：mic_state 字段必须是 types.ts 的 MicState 大写字面量
//   ("IDLE"/"WAITING_TAP"/"ARMED"/"RECORDING"/"PROCESSING")，
//   与 mic_state 帧（小写）的字符串约定不同——wsClient 透传 snapshot 不做大小写转换。
struct WsSnapshotInput {
    // ---- system ----
    int8_t   rssi;             // dBm
    uint32_t uptime_sec;
    const char* ip;            // 不持有，调用期间必须存活
    float    packet_rate;      // pkt/s（端侧自统计）
    int32_t  latency_ms;       // 当前阶段固定 0；前端 ping/pong 测算
    int8_t   battery;          // 当前阶段固定 100；后续接 ADC

    // ---- hands ----
    // 任一手 has_xxx=false 时，对应 hands.left/right 仍发出但 fingers 全 0、imu 全 0。
    // 这样前端 SystemSnapshot 字段始终完整，避免分支处理。
    bool     has_left;
    bool     has_right;
    uint16_t left_flex_raw[5];
    float    left_flex_norm[5];
    bool     left_flex_bent[5];
    float    left_pitch;
    float    left_roll;
    float    left_accel_delta; // |a|-1g
    float    left_gyro_mag;    // °/s
    uint16_t right_flex_raw[5];
    float    right_flex_norm[5];
    bool     right_flex_bent[5];
    float    right_pitch;
    float    right_roll;
    float    right_accel_delta;
    float    right_gyro_mag;

    // ---- mic ----
    const char* mic_state;     // 大写："IDLE"/"WAITING_TAP"/"ARMED"/"RECORDING"/"PROCESSING"
    float    mic_level;        // 0~1
    int32_t  mic_recording_ms; // 当前录音时长 ms；非录音态填 0

    // ---- gesture ----
    bool        gesture_has_top;
    const char* gesture_top_text;       // 仅 has_top=true 时有效
    float       gesture_top_confidence; // 0~1
    const char* gesture_source;         // "left"/"right"/"both"/"none"
    uint32_t    gesture_timestamp;      // ms（millis()）
};

// ---------- 生命周期 ----------

// 启动 WS server（必须在 WiFi 就绪后调用）。返回 false 表示监听失败。
// 多次调用幂等：第二次直接返回 true。
bool WsServer_Init();

// 主循环周期性调用：处理底层 WS 事件 + 心跳。
// 推荐放 loop() 顶部，与 handleSerialCommand() 同级。
void WsServer_Tick();

// 是否至少有一个客户端已 hello。供上层决定要不要烧 base64 / snapshot JSON。
bool WsServer_HasHelloedClient();

// 已连接客户端数量（含未 hello 的）。诊断用。
uint8_t WsServer_GetClientCount();

// 是否至少有一个客户端在 hello 时声明 caps.demoMode=true。
// tts_player 钩子据此决定是否广播 tts_audio。
// 全部客户端断开后自动回 false。
bool WsServer_IsDemoMode();

// ---------- 业务帧广播 ----------

// 推送 mic_state 帧。state_str 取值：'idle' | 'armed' | 'recording' | 'processing'。
// level/recordingMs 任一为 NaN/<0 时该字段省略不发。
void WsServer_BroadcastMicState(const char* state_str,
                                float level,
                                int32_t recording_ms);

// 推送 audio_chunk 帧。pcm = 16-bit LE 单声道；samples = pcm 数组长度。
// seq 由上层维护（每次 mic on 重置）；final=true 表示这是本次录音的末块。
void WsServer_BroadcastAudioChunk(const int16_t* pcm,
                                  size_t samples,
                                  uint32_t seq,
                                  bool final);

// 推送 snapshot 帧。调用前必须自行做：
//   1) millis() 节流（10Hz/5Hz，由调用方按是否在录音中切换间隔）
//   2) WsServer_HasHelloedClient() 检查（无客户端时跳过，避免空载组帧）
// 本函数自身不做节流也不查连接状态，组帧后无条件 broadcast。
void WsServer_BroadcastSnapshot(const WsSnapshotInput& in);

// 推送 tts_audio 帧（演示模式）。pcm = 16-bit LE 单声道；samples = 数组长度。
// seq 由调用方维护（每次 speak/PlayPcmInt16 入口归零并自增）；
// final=true 表示本句 TTS 结束（前端据此释放 AudioContext 队列）。
// text 仅在 seq=0 的首帧填写（"你好" 之类原文，便于前端字幕）；其它帧传 nullptr。
// sample_rate 由调用方提供（speak 通常 16000，离线兜底也是 16000）。
// 内部不做 IsDemoMode 判断；上层钩子负责按需调用，避免空载烧 base64。
void WsServer_BroadcastTtsAudio(const int16_t* pcm,
                                size_t samples,
                                uint32_t seq,
                                bool final,
                                const char* text,
                                uint32_t sample_rate);

#ifdef __cplusplus
}
#endif

#endif  // WS_SERVER_H
