// ============================================================
// ws_server.cpp
// 见 ws_server.h 头部说明。
// 库依赖：WebSockets by Markus Sattler (Links2004/arduino-WebSockets)
// 在 Arduino IDE 通过 "工具 → 管理库 → 搜索 WebSockets 安装"。
// ============================================================
#include "ws_server.h"

#if ENABLE_WS_SERVER

#include <Arduino.h>
#include <WiFi.h>
#include <WebSocketsServer.h>
#include <ArduinoJson.h>
#include <base64.h>

namespace {

WebSocketsServer s_ws(WS_SERVER_PORT);
bool s_initialized = false;

// 每个 num 对应 WebSockets 库分配的 client slot；记录是否已 hello
constexpr uint8_t kMaxClients = WEBSOCKETS_SERVER_CLIENT_MAX;
bool s_helloed[kMaxClients] = {false};
// 客户端 hello 时声明的 caps.demoMode；断开/未 hello 时为 false
bool s_clientDemoMode[kMaxClients] = {false};

// JSON envelope 的最大尺寸：audio_chunk base64 ≈ 1024 byte * 4/3 ≈ 1368 byte，
// 加上 envelope 字段 ~100 byte，留 1.5x 余量。
constexpr size_t kJsonBufBytes = 2400;

// ------------------------------------------------------------
// 构造 envelope JSON 头并把 payload 写入。最后 payload 部分由调用方填充
// 然后 serializeJson 返回字符串。这里统一封装"基础 envelope + payload"。
// ------------------------------------------------------------
String makeEnvelopeJson(const char* kind,
                        std::function<void(JsonObject)> fillPayload) {
    JsonDocument doc;
    doc["v"]    = WS_PROTO_VERSION;
    doc["kind"] = kind;
    doc["ts"]   = (uint32_t)millis();
    JsonObject payload = doc["payload"].to<JsonObject>();
    if (fillPayload) {
        fillPayload(payload);
    }
    String out;
    out.reserve(kJsonBufBytes);
    serializeJson(doc, out);
    return out;
}

void sendHelloTo(uint8_t num) {
    String s = makeEnvelopeJson("hello", [](JsonObject p) {
        p["v"]    = WS_PROTO_VERSION;
        p["role"] = "server";
        JsonObject caps = p["caps"].to<JsonObject>();
        caps["binaryAudio"] = false;  // 当前阶段统一用 base64 文本帧
    });
    s_ws.sendTXT(num, s);
}

void sendPongTo(uint8_t num) {
    String s = makeEnvelopeJson("pong", [](JsonObject /*p*/) {});
    s_ws.sendTXT(num, s);
}

// ------------------------------------------------------------
// 收到客户端文本帧的处理：仅识别 hello / ping / pong / control，
// 其它直接忽略（不解析 audio_chunk 等服务端 → 客户端方向的帧）。
// ------------------------------------------------------------
void handleClientText(uint8_t num, const char* text, size_t len) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, text, len);
    if (err) {
        Serial.printf("[WS] client %u: 收到非 JSON 帧（%u byte），忽略\n",
                      (unsigned)num, (unsigned)len);
        return;
    }
    const char* kind = doc["kind"] | "";
    if (strcmp(kind, "hello") == 0) {
        if (num < kMaxClients) {
            s_helloed[num] = true;
            // caps.demoMode 可选；缺省 false
            bool demo = doc["payload"]["caps"]["demoMode"] | false;
            s_clientDemoMode[num] = demo;
            Serial.printf("[WS] client %u: hello 完成%s，已可推送业务帧\n",
                          (unsigned)num, demo ? "（demoMode=on）" : "");
        }
    } else if (strcmp(kind, "ping") == 0) {
        sendPongTo(num);
    } else if (strcmp(kind, "pong") == 0) {
        // 当前阶段服务端不主动 ping，pong 直接忽略
    } else if (strcmp(kind, "control") == 0) {
        const char* action = doc["payload"]["action"] | "";
        Serial.printf("[WS] client %u: control action=%s（当前未实现）\n",
                      (unsigned)num, action);
    } else {
        Serial.printf("[WS] client %u: 未知 kind=%s，忽略\n", (unsigned)num, kind);
    }
}

void onWsEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
    switch (type) {
        case WStype_CONNECTED: {
            IPAddress ip = s_ws.remoteIP(num);
            Serial.printf("[WS] client %u 已连接 from %s\n",
                          (unsigned)num, ip.toString().c_str());
            if (num < kMaxClients) {
                s_helloed[num] = false;
                s_clientDemoMode[num] = false;
            }
            sendHelloTo(num);  // 服务端先发 hello，等客户端回 hello 才置位
            break;
        }
        case WStype_DISCONNECTED:
            Serial.printf("[WS] client %u 已断开\n", (unsigned)num);
            if (num < kMaxClients) {
                s_helloed[num] = false;
                s_clientDemoMode[num] = false;
            }
            break;
        case WStype_TEXT:
            handleClientText(num, (const char*)payload, length);
            break;
        case WStype_BIN:
            // 当前协议未定义客户端→服务端二进制帧
            Serial.printf("[WS] client %u: 收到二进制帧 %u byte，忽略\n",
                          (unsigned)num, (unsigned)length);
            break;
        case WStype_ERROR:
            Serial.printf("[WS] client %u: error\n", (unsigned)num);
            break;
        default:
            break;
    }
}

}  // namespace

bool WsServer_Init() {
    if (s_initialized) return true;
    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[WS] 警告: WiFi 未就绪，WS server 暂不启动");
        return false;
    }
    s_ws.begin();
    s_ws.onEvent(onWsEvent);
    s_initialized = true;
    Serial.printf("[WS] 监听 ws://%s:%u%s\n",
                  WiFi.localIP().toString().c_str(),
                  (unsigned)WS_SERVER_PORT,
                  WS_SERVER_PATH);
    return true;
}

void WsServer_Tick() {
    if (!s_initialized) return;
    s_ws.loop();
}

bool WsServer_HasHelloedClient() {
    if (!s_initialized) return false;
    for (uint8_t i = 0; i < kMaxClients; i++) {
        if (s_helloed[i]) return true;
    }
    return false;
}

uint8_t WsServer_GetClientCount() {
    if (!s_initialized) return 0;
    return s_ws.connectedClients();
}

bool WsServer_IsDemoMode() {
    if (!s_initialized) return false;
    for (uint8_t i = 0; i < kMaxClients; i++) {
        if (s_helloed[i] && s_clientDemoMode[i]) return true;
    }
    return false;
}

void WsServer_BroadcastMicState(const char* state_str,
                                float level,
                                int32_t recording_ms) {
    if (!s_initialized) return;
    String s = makeEnvelopeJson("mic_state", [&](JsonObject p) {
        p["state"] = state_str ? state_str : "idle";
        if (!isnan(level) && level >= 0.0f) {
            p["level"] = level;
        }
        if (recording_ms >= 0) {
            p["recordingMs"] = (uint32_t)recording_ms;
        }
    });
    s_ws.broadcastTXT(s);
}

void WsServer_BroadcastAudioChunk(const int16_t* pcm,
                                  size_t samples,
                                  uint32_t seq,
                                  bool final) {
    if (!s_initialized || !pcm || samples == 0) {
        // final=true 时即便 samples=0 也要发一条空帧，告诉前端录音结束
        if (s_initialized && final) {
            String s = makeEnvelopeJson("audio_chunk", [&](JsonObject p) {
                p["seq"]           = seq;
                p["sampleRate"]    = MIC_SAMPLE_RATE_HZ;
                p["bitsPerSample"] = MIC_BITS_PER_SAMPLE;
                p["channels"]      = MIC_CHANNELS;
                p["pcmBase64"]     = "";
                p["final"]         = true;
            });
            s_ws.broadcastTXT(s);
        }
        return;
    }
    // 16-bit LE：ESP32 是小端，直接 reinterpret 即可
    const size_t bytes = samples * sizeof(int16_t);
    String b64 = base64::encode(reinterpret_cast<const uint8_t*>(pcm), bytes);
    String s = makeEnvelopeJson("audio_chunk", [&](JsonObject p) {
        p["seq"]           = seq;
        p["sampleRate"]    = MIC_SAMPLE_RATE_HZ;
        p["bitsPerSample"] = MIC_BITS_PER_SAMPLE;
        p["channels"]      = MIC_CHANNELS;
        p["pcmBase64"]     = b64;
        p["final"]         = final;
    });
    s_ws.broadcastTXT(s);
}

// ------------------------------------------------------------
// snapshot 帧组装
// ------------------------------------------------------------
// 字段名 / 节点结构与 LingxiGlove_APP types.ts SystemSnapshot 严格对齐：
//   system: { connectionStatus, rssi, battery, latencyMs, uptimeSec, packetRate, ip }
//   hands:  { left:  { fingers: { 拇/食/中/无/小: { raw, normalized, bent } }, imu: { pitch, roll, accelDelta, gyroMag } },
//             right: ... }
//   mic:    { state, level, spectrum: [], recordingMs }
//   gesture:{ candidates: [...], source, timestamp }
// 注意 fingers 用中文 key（FINGER_NAMES = ['拇','食','中','无','小']），
// ArduinoJson v7 支持 UTF-8 字符串作 key，无需特殊处理。
namespace {

// fingers key：UTF-8 编码的中文（每个字 3 byte）
constexpr const char* kFingerKeys[5] = {"拇", "食", "中", "无", "小"};

// 把单只手的 fingers + imu 节点写入 hand JsonObject。
void fillHandNode(JsonObject hand,
                  bool has_data,
                  const uint16_t flex_raw[5],
                  const float flex_norm[5],
                  const bool flex_bent[5],
                  float pitch, float roll,
                  float accel_delta, float gyro_mag) {
    JsonObject fingers = hand["fingers"].to<JsonObject>();
    for (int i = 0; i < 5; i++) {
        JsonObject f = fingers[kFingerKeys[i]].to<JsonObject>();
        if (has_data) {
            f["raw"]        = flex_raw[i];
            f["normalized"] = flex_norm[i];
            f["bent"]       = flex_bent[i];
        } else {
            // 占位：与 EMPTY_SNAPSHOT 一致（raw=2800 表示满量程伸直）
            f["raw"]        = 2800;
            f["normalized"] = 0.0f;
            f["bent"]       = false;
        }
    }
    JsonObject imu = hand["imu"].to<JsonObject>();
    if (has_data) {
        imu["pitch"]      = pitch;
        imu["roll"]       = roll;
        imu["accelDelta"] = accel_delta;
        imu["gyroMag"]    = gyro_mag;
    } else {
        imu["pitch"] = 0.0f;
        imu["roll"]  = 0.0f;
    }
}

}  // namespace

void WsServer_BroadcastSnapshot(const WsSnapshotInput& in) {
    if (!s_initialized) return;
    String s = makeEnvelopeJson("snapshot", [&](JsonObject p) {
        // ---- system ----
        JsonObject sys = p["system"].to<JsonObject>();
        // 注意：不写 connectionStatus / latencyMs。
        //   connectionStatus 由 APP 端基于 WS open/close 状态计算（端侧从自己的视角
        //   只能永远说 "connected"，反而会覆盖掉 reconnecting/disconnected 等过渡状态）。
        //   latencyMs 由 APP 端 ping/pong RTT 测算，端侧无法替它打这个值。
        sys["rssi"]             = in.rssi;
        sys["battery"]          = in.battery;
        sys["latencyMs"]        = in.latency_ms;
        sys["uptimeSec"]        = in.uptime_sec;
        sys["packetRate"]       = in.packet_rate;
        sys["ip"]               = in.ip ? in.ip : "0.0.0.0";

        // ---- hands ----
        JsonObject hands = p["hands"].to<JsonObject>();
        JsonObject left  = hands["left"].to<JsonObject>();
        JsonObject right = hands["right"].to<JsonObject>();
        fillHandNode(left, in.has_left,
                     in.left_flex_raw, in.left_flex_norm, in.left_flex_bent,
                     in.left_pitch, in.left_roll,
                     in.left_accel_delta, in.left_gyro_mag);
        fillHandNode(right, in.has_right,
                     in.right_flex_raw, in.right_flex_norm, in.right_flex_bent,
                     in.right_pitch, in.right_roll,
                     in.right_accel_delta, in.right_gyro_mag);

        // ---- mic ----
        JsonObject mic = p["mic"].to<JsonObject>();
        mic["state"]       = in.mic_state ? in.mic_state : "IDLE";
        mic["level"]       = in.mic_level;
        // spectrum 端侧暂不实现，发空数组让前端跳过频谱绘制
        mic["spectrum"].to<JsonArray>();
        mic["recordingMs"] = in.mic_recording_ms < 0 ? 0 : in.mic_recording_ms;

        // ---- gesture ----
        JsonObject ges = p["gesture"].to<JsonObject>();
        JsonArray cands = ges["candidates"].to<JsonArray>();
        if (in.gesture_has_top && in.gesture_top_text) {
            JsonObject c = cands.add<JsonObject>();
            c["text"]       = in.gesture_top_text;
            c["confidence"] = in.gesture_top_confidence;
        }
        ges["source"]    = in.gesture_source ? in.gesture_source : "none";
        ges["timestamp"] = in.gesture_timestamp;
    });
    s_ws.broadcastTXT(s);
}

// ------------------------------------------------------------
// tts_audio 帧（演示模式专用）
// ------------------------------------------------------------
// 与 audio_chunk 同构：seq/sampleRate/bitsPerSample/channels/pcmBase64/final，
// 额外在 seq=0 时带 text 字段。前端按 seq 顺序拼接到 AudioContext 队列。
// 调用约束（tts_player 钩子）：
//   - speak() 入口前 seq=0；每次 i2s_write 前自增；最后一次 i2s_write 后调用一次
//     samples=0/final=true 的"收尾帧"，前端释放队列。
//   - 也允许"末块自带 final"（与 audio_chunk 末块语义一致），由调用方选其一。
//   - 内部不查 IsDemoMode/HasHelloedClient，必须由钩子先判断后再调用。
void WsServer_BroadcastTtsAudio(const int16_t* pcm,
                                size_t samples,
                                uint32_t seq,
                                bool final,
                                const char* text,
                                uint32_t sample_rate) {
    if (!s_initialized) return;
    // 收尾空帧：samples=0 + final=true 直接发空 base64
    if (!pcm || samples == 0) {
        if (final) {
            String s = makeEnvelopeJson("tts_audio", [&](JsonObject p) {
                p["seq"]           = seq;
                p["sampleRate"]    = sample_rate ? sample_rate : 16000;
                p["bitsPerSample"] = 16;
                p["channels"]      = 1;
                p["pcmBase64"]     = "";
                p["final"]         = true;
                if (text && text[0]) p["text"] = text;
            });
            s_ws.broadcastTXT(s);
        }
        return;
    }
    const size_t bytes = samples * sizeof(int16_t);
    String b64 = base64::encode(reinterpret_cast<const uint8_t*>(pcm), bytes);
    String s = makeEnvelopeJson("tts_audio", [&](JsonObject p) {
        p["seq"]           = seq;
        p["sampleRate"]    = sample_rate ? sample_rate : 16000;
        p["bitsPerSample"] = 16;
        p["channels"]      = 1;
        p["pcmBase64"]     = b64;
        p["final"]         = final;
        if (text && text[0]) p["text"] = text;  // 通常仅 seq=0 时填写
    });
    s_ws.broadcastTXT(s);
}

#else  // !ENABLE_WS_SERVER

// ENABLE_WS_SERVER = 0 时全部 no-op，避免上层 #if 包裹散落
bool WsServer_Init() { return false; }
void WsServer_Tick() {}
bool WsServer_HasHelloedClient() { return false; }
uint8_t WsServer_GetClientCount() { return 0; }
bool WsServer_IsDemoMode() { return false; }
void WsServer_BroadcastMicState(const char*, float, int32_t) {}
void WsServer_BroadcastAudioChunk(const int16_t*, size_t, uint32_t, bool) {}
void WsServer_BroadcastSnapshot(const WsSnapshotInput&) {}
void WsServer_BroadcastTtsAudio(const int16_t*, size_t, uint32_t, bool,
                                const char*, uint32_t) {}

#endif  // ENABLE_WS_SERVER
