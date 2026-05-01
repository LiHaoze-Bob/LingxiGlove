#include "tts_player.h"
#include "config.h"
#include "http_client.h"
#include "llm_client.h"
#include <driver/i2s.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <math.h>

static bool s_i2sInitialized = false;

// ----------------------------------------------------------------------
// 工具函数：饱和软件增益
// ----------------------------------------------------------------------
// 对 uint8 缓冲区里的 int16 PCM 样本乘以 TTS_VOLUME_GAIN，使用饱和截幅
// 防止乘法溢出导致波形翻转（会出现"嘶嘶"杂音）。
// 要求缓冲区字节数为偶数（WAV / I2S 数据天然满足此条件）。
static void ApplyGain(uint8_t* buf, size_t byte_count) {
    // TTS_VOLUME_GAIN == 1.0 时此函数体为空，编译器会内联优化为无操作。
    // 预处理器不支持浮点比较，所以不用 #if 条件，直接依赖编译器常量折叠。
    const float gain = TTS_VOLUME_GAIN;
    if (gain == 1.0f) return;  // 编译器对常量值会优化掉此分支
    int16_t* samples = reinterpret_cast<int16_t*>(buf);
    size_t sample_count = byte_count / sizeof(int16_t);
    for (size_t i = 0; i < sample_count; ++i) {
        float amplified = samples[i] * gain;
        if (amplified >  32767.0f) amplified =  32767.0f;
        if (amplified < -32768.0f) amplified = -32768.0f;
        samples[i] = static_cast<int16_t>(amplified);
    }
}

// ----------------------------------------------------------------------
// 工具函数：标准 RIFF WAV 头解析
// ----------------------------------------------------------------------
// Qwen-TTS 返回的 audio.url 指向一个 .wav 文件。标准 PCM WAV 的前 44 字节
// 是 RIFF 头，随后才是 int16 PCM 数据。本函数只做最小必要解析：
//   - 校验 "RIFF" / "WAVE" / "fmt "
//   - 取出 sample_rate、num_channels、bits_per_sample
//   - 跳到 "data" chunk 起点并返回其负载字节数
// 不支持扩展 fmt chunk 或非 PCM 编码（Qwen-TTS 实测均为 24kHz/16bit/Mono
// 标准 PCM，足够覆盖；如未来出现 44 字节之外的 fmt 扩展，此函数会返回
// 失败而不是盲目前进）。
//
// @param client   已连接且正在接收该 WAV 的 WiFiClient；函数会从中消耗前若干字节
// @param out_rate 解析到的采样率
// @param out_ch   解析到的声道数
// @param out_bps  解析到的位深
// @param out_data_bytes data chunk 的字节长度（单位 Byte，不是 Sample）
// @return true 头有效；false 任一字段非法或流意外结束
static bool ReadWavHeader(WiFiClient* client,
                          uint32_t* out_rate,
                          uint16_t* out_ch,
                          uint16_t* out_bps,
                          uint32_t* out_data_bytes) {
    if (client == nullptr || out_rate == nullptr ||
        out_ch == nullptr || out_bps == nullptr ||
        out_data_bytes == nullptr) {
        return false;
    }

    // 小工具：阻塞读固定字节数（带整体超时，避免网络半关时死等）
    const unsigned long kHeaderReadTimeoutMs = 5000;
    auto read_exact = [&](uint8_t* dst, size_t n) -> bool {
        unsigned long start = millis();
        size_t got = 0;
        while (got < n) {
            if (client->available() > 0) {
                int r = client->read(dst + got, n - got);
                if (r > 0) {
                    got += (size_t)r;
                }
            } else {
                if (millis() - start > kHeaderReadTimeoutMs) return false;
                delay(2);
            }
        }
        return true;
    };

    uint8_t riff[12];
    if (!read_exact(riff, sizeof(riff))) return false;
    if (riff[0] != 'R' || riff[1] != 'I' || riff[2] != 'F' || riff[3] != 'F' ||
        riff[8] != 'W' || riff[9] != 'A' || riff[10]!= 'V' || riff[11]!= 'E') {
        return false;
    }

    // 后续是若干 chunk："fmt "（含音频参数）+ 可能的 "LIST" 等 + "data"
    // 循环解析直到找到 "data"
    const int kMaxChunks = 8;  // 防御无限循环
    for (int i = 0; i < kMaxChunks; ++i) {
        uint8_t chunk_header[8];
        if (!read_exact(chunk_header, sizeof(chunk_header))) return false;
        uint32_t size = (uint32_t)chunk_header[4]
                      | ((uint32_t)chunk_header[5] << 8)
                      | ((uint32_t)chunk_header[6] << 16)
                      | ((uint32_t)chunk_header[7] << 24);

        if (chunk_header[0] == 'f' && chunk_header[1] == 'm' &&
            chunk_header[2] == 't' && chunk_header[3] == ' ') {
            // fmt chunk：至少 16 字节
            if (size < 16) return false;
            uint8_t fmt[16];
            if (!read_exact(fmt, sizeof(fmt))) return false;
            uint16_t audio_format = (uint16_t)fmt[0] | ((uint16_t)fmt[1] << 8);
            if (audio_format != 1) {
                // 1 = PCM；其他编码本函数不支持
                return false;
            }
            *out_ch   = (uint16_t)fmt[2]  | ((uint16_t)fmt[3]  << 8);
            *out_rate = (uint32_t)fmt[4]  | ((uint32_t)fmt[5]  << 8)
                      | ((uint32_t)fmt[6] << 16) | ((uint32_t)fmt[7] << 24);
            *out_bps  = (uint16_t)fmt[14] | ((uint16_t)fmt[15] << 8);
            // 如果 fmt 段还有扩展，消费掉，保持流对齐
            uint32_t extra = size - 16;
            while (extra > 0) {
                uint8_t skip[32];
                size_t once = (extra > sizeof(skip)) ? sizeof(skip) : (size_t)extra;
                if (!read_exact(skip, once)) return false;
                extra -= once;
            }
        } else if (chunk_header[0] == 'd' && chunk_header[1] == 'a' &&
                   chunk_header[2] == 't' && chunk_header[3] == 'a') {
            // Qwen-TTS 返回的是"流式 WAV"：服务端生成时不知总长度，
            // 所以 data chunk size 字段填了 0xFFFFFFFF 或接近 INT32_MAX 的占位值。
            // 检测到超大值（> 4 MB）时归一化为 0，让调用方以"流式未知长度"模式播放，
            // 靠 HTTP 连接断开 + 空读超时来决定何时结束，而不依赖 remaining 计数。
            const uint32_t kMaxReasonableWavBytes = 4u * 1024u * 1024u;  // 4 MB
            *out_data_bytes = (size > kMaxReasonableWavBytes) ? 0u : size;
            return true;
        } else {
            // 其他 chunk（"LIST"/"bext"/...）：跳过
            uint32_t extra = size;
            while (extra > 0) {
                uint8_t skip[64];
                size_t once = (extra > sizeof(skip)) ? sizeof(skip) : (size_t)extra;
                if (!read_exact(skip, once)) return false;
                extra -= once;
            }
        }
    }
    return false;
}

bool initTTS() {
    if (s_i2sInitialized) return true;

    // DMA 缓冲容量选型（抗 WiFi 抖动的核心）：
    //   dma_buf_count=16, dma_buf_len=1024 samples → 总深度 32 KB / 16-bit Mono
    //   换算时长：24 kHz 下约 680 ms，16 kHz 下约 1.0 s
    //   → 即使 WiFi 重传导致 500 ms 网络停顿，DMA 队列也仍有数据喂功放，不会
    //     出现播放静音帧（表现为"咔/顿"）。
    //   SRAM 开销：32 KB（ESP32-S3 内部 512 KB SRAM 足够），生命周期覆盖整个系统运行。
    i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
        .sample_rate = 24000,  // 与 Qwen-TTS 输出一致，消除采样率切换杂音
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 16,
        .dma_buf_len = 1024,
        .use_apll = false,
        .tx_desc_auto_clear = true,
        .fixed_mclk = 0
    };

    i2s_pin_config_t pin_config = {
        .bck_io_num = I2S_BCLK,
        .ws_io_num = I2S_LRC,
        .data_out_num = I2S_DIN,
        .data_in_num = I2S_PIN_NO_CHANGE
    };

    esp_err_t err = i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
    if (err != ESP_OK) {
        DEBUG_PRINTLN("[TTS] I2S驱动安装失败");
        return false;
    }

    err = i2s_set_pin(I2S_NUM_0, &pin_config);
    if (err != ESP_OK) {
        DEBUG_PRINTLN("[TTS] I2S引脚配置失败");
        i2s_driver_uninstall(I2S_NUM_0);
        return false;
    }

    s_i2sInitialized = true;
    DEBUG_PRINTLN("[TTS] I2S初始化成功 (24kHz, 16bit, Mono)");
    return true;
}


// ----------------------------------------------------------------------
// 工具函数：轻量 base64 解码器
// ----------------------------------------------------------------------
// Qwen-TTS SSE 流式模式下，每帧 audio.data 是 base64 编码的 int16 PCM。
// 本实现是标准 RFC 4648 base64 解码，不依赖任何外部库：
//   - 忽略 '=' 填充字符
//   - 非法字符跳过（健壮性处理）
//   - 输出字节数 = floor(valid_input_chars / 4) * 3 + remainder
//
// @param src       base64 编码字符串（可不以 '\0' 结尾，由 src_len 控制）
// @param src_len   base64 字符串长度
// @param dst       解码输出缓冲区
// @param dst_len   输出缓冲区容量（需 >= src_len * 3 / 4）
// @return 实际解码写出的字节数
static size_t Base64Decode(const char* src, size_t src_len, uint8_t* dst, size_t dst_len) {
    static const int8_t kDecTable[256] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
        52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-2,-1,-1,
        -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
        15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
        41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
    };
    size_t out = 0;
    uint32_t accum = 0;
    int bits = 0;
    for (size_t i = 0; i < src_len && out < dst_len; ++i) {
        int8_t v = kDecTable[(uint8_t)src[i]];
        if (v < 0) continue;  // 跳过 '='（-2）和非法字符（-1）
        accum = (accum << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            dst[out++] = (uint8_t)(accum >> bits);
        }
    }
    return out;
}

/**
 * @brief 云端 TTS：通过阿里 DashScope Qwen-TTS SSE 流式合成并播报。
 *
 * 新方案（相比旧两步 HTTP，延迟从 3-8s 降至 0.5-1s）：
 *   - 旧方案：POST → 等服务端全段合成完 → 返回 audio.url → 再次 GET 下载 WAV
 *             → 流式写 I2S（两次 TLS 握手，等待全段合成是主要瓶颈）
 *   - 新方案：POST（加 X-DashScope-SSE: enable 头）→ 服务端逐帧推送
 *             base64 编码的 PCM 分片 → 边收边解码边写 I2S
 *             （单次 TLS 握手，首帧到达即可出声）
 *
 * SSE 响应格式（DashScope Qwen-TTS 流式）：
 *   每帧：data: {"output":{"audio":{"data":"BASE64_PCM","url":null},"finish_reason":"null"}}
 *   末帧：data: {"output":{"audio":{"data":"","url":"https://..."},"finish_reason":"stop"}}
 *
 * audio.data 为 base64 编码的 int16 PCM，格式固定 24kHz/16bit/Mono，
 * 与 QWEN_TTS_SAMPLE_RATE 定义一致，无需解析 WAV 头。
 *
 * 内存约束：
 *   PCM 分片解码缓冲区（s_pcm_decode_buf）使用 static 放 .bss 段，
 *   容量 6144 字节对应约 80ms 24kHz 音频，覆盖单帧最大尺寸。
 *   不在栈上分配，避免触发 "Stack canary watchpoint triggered" 复位。
 *
 * @param text 要朗读的文本，非空且长度 <= 600 字符（Qwen-TTS 上限）
 * @return true 播放完成；false 任一环节失败（调用方可回落到 local_tts_fallback）
 */
bool speak(const char* text) {
    if (!s_i2sInitialized) {
        DEBUG_PRINTLN("[TTS] 错误: I2S 未初始化");
        return false;
    }
    if (text == nullptr || text[0] == '\0') {
        DEBUG_PRINTLN("[TTS] 错误: 文本为空");
        return false;
    }
    const size_t kMaxTextBytes = 600 * 3;  // 中文 UTF-8 最多 3 字节/字
    size_t text_len = strlen(text);
    if (text_len > kMaxTextBytes) {
        DEBUG_LOG("[TTS] 错误: 文本过长 (bytes=%u)", (uint32_t)text_len);
        return false;
    }
    if (!WiFi.isConnected()) {
        DEBUG_PRINTLN("[TTS] 错误: WiFi 未连接");
        return false;
    }

    // -------- 构建 SSE 流式 POST 请求 --------
    JsonDocument req_doc;
    req_doc["model"] = QWEN_TTS_MODEL;
    JsonObject input = req_doc["input"].to<JsonObject>();
    input["text"]          = text;
    input["voice"]         = QWEN_TTS_VOICE;
    input["language_type"] = QWEN_TTS_LANGUAGE;

    String payload;
    payload.reserve(256 + text_len);
    serializeJson(req_doc, payload);

    String auth_header = "Bearer ";
    auth_header += QWEN_API_KEY;

    DEBUG_LOG("[TTS] 请求合成(SSE流式): %s", text);

    // -------- 预切 I2S 采样率到 24kHz --------
    // SSE 模式下无 WAV 头可解析，采样率由 QWEN_TTS_SAMPLE_RATE 常量保证。
    // 切换前必须先清零 DMA 缓冲，消除上次播放的残留数据；
    // 否则旧数据会以新采样率输出，产生"滋滋"杂音。
    bool rate_switched = false;
    if (QWEN_TTS_SAMPLE_RATE != TTS_I2S_DEFAULT_SAMPLE_RATE) {
        i2s_zero_dma_buffer(I2S_NUM_0);  // 清零 DMA 缓冲，防止残留数据产生杂音
        esp_err_t err = i2s_set_sample_rates(I2S_NUM_0, QWEN_TTS_SAMPLE_RATE);
        if (err != ESP_OK) {
            DEBUG_LOG("[TTS] 错误: 预切采样率失败 err=%d", (int)err);
            return false;
        }
        rate_switched = true;
    }

    // -------- SSE 接收：全量攒 PCM，不在回调里写 I2S --------
    //
    // 【重要设计决策：为何不在回调里实时写 I2S】
    // 回调里 i2s_write(portMAX_DELAY) 在 DMA 满时会阻塞整个任务，
    // 阻塞期间 httpPostJsonSse 的 TCP 读循环完全停止 → TCP 接收窗口收缩
    // → 服务端推送降速 → 每帧间隔被拉长，形成恶性循环。
    // 实测：理论 0.76s 的音频实际耗时 9s（相差 12×），首帧 pcm=6118B
    // 也证明了服务端多帧数据堆积在 TCP buffer 里等待被读。
    //
    // 新方案：回调里只做 base64 解码 + memcpy 到 s_pcm_accum_buf，
    // SSE 全量读完后再统一写 I2S，TCP 读取不受 I2S DMA 阻塞影响。
    //
    // s_pcm_accum_buf 容量：60000 B ≈ 1.25s @ 24kHz/16bit/Mono，
    // 覆盖绝大多数短句；超长句子截断（不影响 MVP 演示场景）。
    // static 放 .bss 段，避免 loopTask 栈（8KB）溢出。
    static uint8_t s_pcm_accum_buf[60000];
    static uint8_t s_frame_decode_buf[6144];  // 单帧 base64 解码临时缓冲
    size_t accum_len = 0;
    bool stream_finished = false;

    auto on_sse_line = [&](const String& line) -> bool {
        // SSE 格式：跳过非 data: 开头的行（注释行、空行、id:/event: 等）
        if (!line.startsWith("data:")) {
            return true;
        }
        const char* json_str = line.c_str() + 5;
        while (*json_str == ' ') ++json_str;

        // [DONE] 标志（部分 SSE 实现会额外发送）
        if (strcmp(json_str, "[DONE]") == 0) {
            stream_finished = true;
            return false;
        }

        // 检测 finish_reason（用 strstr 避免把 5KB+ base64 字符串塞入 JsonDocument 堆）
        if (strstr(json_str, "\"finish_reason\":\"stop\"") != nullptr) {
            stream_finished = true;
        }

        // 提取 audio.data（base64 PCM）：用 strstr 直接定位，避免 JsonDocument 复制大字符串
        const char* data_key = strstr(json_str, "\"data\":\"");
        if (data_key == nullptr) {
            return !stream_finished;
        }
        const char* b64_start = data_key + 8;  // 跳过 "data":"
        if (*b64_start == '"') {
            // data="" 空串，末帧正常现象
            return !stream_finished;
        }

        // 找 base64 字符串结尾（下一个未转义的 '"'）
        const char* b64_end = b64_start;
        while (*b64_end != '\0' && *b64_end != '"') {
            if (*b64_end == '\\') b64_end++;  // 跳过转义字符
            if (*b64_end != '\0') b64_end++;
        }
        size_t b64_len = (size_t)(b64_end - b64_start);
        if (b64_len == 0) {
            return !stream_finished;
        }

        // base64 解码到临时帧缓冲
        size_t pcm_bytes = Base64Decode(b64_start, b64_len,
                                        s_frame_decode_buf, sizeof(s_frame_decode_buf));
        if (pcm_bytes == 0) {
            return !stream_finished;
        }

        // 追加到累积缓冲（超出容量时截断，记录警告）
        size_t space = sizeof(s_pcm_accum_buf) - accum_len;
        if (pcm_bytes > space) {
            DEBUG_LOG("[TTS-SSE] 警告: 累积缓冲将满, 截断 %u B", (uint32_t)(pcm_bytes - space));
            pcm_bytes = space;
        }
        if (pcm_bytes > 0) {
            memcpy(s_pcm_accum_buf + accum_len, s_frame_decode_buf, pcm_bytes);
            accum_len += pcm_bytes;
        }

        return !stream_finished;
    };

    DEBUG_LOG("[TTS-SSE] 开始接收 SSE 音频流...");
    int http_code = httpPostJsonSse(QWEN_TTS_ENDPOINT, payload,
                                    auth_header.c_str(), on_sse_line);
    DEBUG_LOG("[TTS-SSE] SSE 接收完成, 累积 PCM=%u B, HTTP=%d",
              (uint32_t)accum_len, http_code);

    // -------- 统一写 I2S 播放（SSE 读取已结束，不再有 TCP 竞争）--------
    bool got_any_audio = false;
    size_t total_written = 0;

    if (accum_len > 0) {
        // 软件增益（gain=1.0 时编译器优化为空）
        ApplyGain(s_pcm_accum_buf, accum_len);

        // 启动 I2S 时钟：i2s_start 之前 MAX98357A 处于静音状态（无 BCLK 时自动静音）。
        // 播放前再 start，确保 DMA 缓冲是干净的零值，消除启动时的"噗"声。
        i2s_zero_dma_buffer(I2S_NUM_0);
        i2s_start(I2S_NUM_0);

        // 分块写 I2S，每块 4096 字节对齐 DMA 描述符大小
        const size_t kI2SChunk = 4096;
        size_t offset = 0;
        while (offset < accum_len) {
            size_t chunk = accum_len - offset;
            if (chunk > kI2SChunk) chunk = kI2SChunk;
            size_t bytes_written = 0;
            i2s_write(I2S_NUM_0, s_pcm_accum_buf + offset, chunk,
                      &bytes_written, portMAX_DELAY);
            total_written += bytes_written;
            offset += chunk;
        }
        got_any_audio = true;
        DEBUG_LOG("[TTS] I2S 写入完成, %u bytes", (uint32_t)total_written);

        // 精确等待 DMA 排空：
        //   等待时间 = 实际写入字节 / (采样率 × 字节/样本)
        //   加上 DMA 缓冲深度对应的时长，确保最后一帧完全输出到功放。
        //   DMA 深度：16 × 1024 samples × 2 bytes = 32768 B @24kHz ≈ 682ms
        uint32_t play_ms = (uint32_t)((total_written * 1000UL) / (QWEN_TTS_SAMPLE_RATE * 2));
        uint32_t dma_drain_ms = (uint32_t)(32768UL * 1000UL / (QWEN_TTS_SAMPLE_RATE * 2));
        uint32_t wait_ms = (play_ms < dma_drain_ms) ? dma_drain_ms : play_ms;
        wait_ms += 100;  // 额外余量，防止末尾被截断
        DEBUG_LOG("[TTS] 等待 DMA 排空 %u ms (播放 %u ms + DMA %u ms)",
                  wait_ms, play_ms, dma_drain_ms);
        delay(wait_ms);

        // 停止 I2S 时钟：让 MAX98357A 自动进入静音状态，消除结尾"滋滋"杂音。
        // MAX98357A 特性：检测到 BCLK 停止后约 1ms 内自动关闭输出级。
        i2s_stop(I2S_NUM_0);
    }

    // rate_switched 现在永远是 false（QWEN_TTS_SAMPLE_RATE == TTS_I2S_DEFAULT_SAMPLE_RATE == 24kHz）
    // 保留此块以防未来配置变更
    if (rate_switched) {
        i2s_zero_dma_buffer(I2S_NUM_0);
        esp_err_t err = i2s_set_sample_rates(I2S_NUM_0, TTS_I2S_DEFAULT_SAMPLE_RATE);
        if (err != ESP_OK) {
            DEBUG_LOG("[TTS] 警告: 恢复默认采样率失败 err=%d", (int)err);
        }
    }

    DEBUG_LOG("[TTS] SSE流式播放完成, HTTP=%d, 写入=%u bytes, 收到音频=%s",
              http_code, (uint32_t)total_written, got_any_audio ? "是" : "否");

    if (http_code != 200) {
        DEBUG_LOG("[TTS] 错误: HTTP状态码=%d", http_code);
        return false;
    }

    return got_any_audio;
}

bool PlayPcmInt16(const int16_t* pcm, size_t sample_count, uint32_t sample_rate) {
    if (!s_i2sInitialized) {
        DEBUG_PRINTLN("[TTS] PlayPcmInt16: I2S 未初始化");
        return false;
    }
    if (!pcm) {
        DEBUG_PRINTLN("[TTS] PlayPcmInt16: pcm 指针为空");
        return false;
    }
    if (sample_count == 0) {
        DEBUG_PRINTLN("[TTS] PlayPcmInt16: sample_count=0");
        return false;
    }
    if (sample_rate < 8000u || sample_rate > 48000u) {
        DEBUG_LOG("[TTS] PlayPcmInt16: sample_rate 越界: %u", sample_rate);
        return false;
    }
    // 最长播放 10 秒，避免传入巨大数组阻塞主循环过久
    const size_t kMaxSamples = (size_t)sample_rate * 10u;
    if (sample_count > kMaxSamples) {
        DEBUG_LOG("[TTS] PlayPcmInt16: sample_count 超过 10 秒上限: %u", (uint32_t)sample_count);
        return false;
    }

    // speak() 播放完后会调 i2s_stop()，此处必须先 start 才能写入。
    // i2s_start 对已运行的 I2S 是幂等的（重复调用无副作用）。
    i2s_zero_dma_buffer(I2S_NUM_0);
    i2s_start(I2S_NUM_0);

    // 如采样率与默认不同，临时切换
    bool rate_switched = false;
    if (sample_rate != TTS_I2S_DEFAULT_SAMPLE_RATE) {
        esp_err_t err = i2s_set_sample_rates(I2S_NUM_0, sample_rate);
        if (err != ESP_OK) {
            DEBUG_LOG("[TTS] PlayPcmInt16: i2s_set_sample_rates 失败 err=%d", (int)err);
            i2s_stop(I2S_NUM_0);
            return false;
        }
        rate_switched = true;
    }

    // 分块写 I2S（一次写太大会阻塞任务切换；每块 512 样本 = 1024 字节对齐 DMA buf）
    // PlayPcmInt16 接受 const int16_t*（通常来自 Flash 只读段），不能原地修改。
    // 软件增益需要可写缓冲：把 chunk 拷贝到栈上临时缓冲再 ApplyGain，
    // 512 样本 × 2 字节 = 1024 字节，在 8 KB loopTask 栈上安全。
    const size_t kChunkSamples = 512;
    int16_t gain_buf[kChunkSamples];
    size_t written_samples = 0;
    while (written_samples < sample_count) {
        size_t chunk = sample_count - written_samples;
        if (chunk > kChunkSamples) chunk = kChunkSamples;
        memcpy(gain_buf, pcm + written_samples, chunk * sizeof(int16_t));
        ApplyGain(reinterpret_cast<uint8_t*>(gain_buf), chunk * sizeof(int16_t));
        size_t bytes_written = 0;
        esp_err_t err = i2s_write(I2S_NUM_0,
                                  gain_buf,
                                  chunk * sizeof(int16_t),
                                  &bytes_written,
                                  portMAX_DELAY);
        if (err != ESP_OK) {
            DEBUG_LOG("[TTS] PlayPcmInt16: i2s_write 失败 err=%d", (int)err);
            if (rate_switched) {
                i2s_set_sample_rates(I2S_NUM_0, TTS_I2S_DEFAULT_SAMPLE_RATE);
            }
            i2s_stop(I2S_NUM_0);
            return false;
        }
        written_samples += bytes_written / sizeof(int16_t);
        // 让出给看门狗
        if (written_samples % (kChunkSamples * 4) == 0) {
            delay(1);
        }
    }

    // 等 DMA 排空，然后停止 I2S 让 MAX98357A 自动静音
    delay(100);

    if (rate_switched) {
        i2s_set_sample_rates(I2S_NUM_0, TTS_I2S_DEFAULT_SAMPLE_RATE);
    }
    i2s_stop(I2S_NUM_0);
    return true;
}

void playTestTone(int freq, int durationMs) {
    if (!s_i2sInitialized) return;

    // 频率/时长边界保护：防止除零或参数异常导致的死循环
    if (freq < 50 || freq > 8000 || durationMs <= 0) {
        DEBUG_PRINTLN("[TTS] 测试音参数非法 (freq 应在 50~8000Hz)");
        return;
    }

    DEBUG_LOG("[TTS] 播放测试音 %dHz, 持续 %dms", freq, durationMs);

    const int sampleRate = 24000;  // 与 I2S 默认采样率一致
    const int samplesPerPeriod = sampleRate / freq;
    if (samplesPerPeriod < 2) {
        DEBUG_PRINTLN("[TTS] 频率过高，无法生成测试音");
        return;
    }

    // speak() 播放后 i2s_stop()，此处必须先 start
    i2s_zero_dma_buffer(I2S_NUM_0);
    i2s_start(I2S_NUM_0);

    int16_t sample;
    size_t bytesWritten = 0;
    unsigned long startTime = millis();

    while (millis() - startTime < (unsigned long)durationMs) {
        for (int i = 0; i < samplesPerPeriod; i++) {
            sample = (int16_t)(sin(2.0 * PI * i / samplesPerPeriod) * 2000);
            i2s_write(I2S_NUM_0, &sample, sizeof(sample), &bytesWritten, portMAX_DELAY);
        }
    }

    // 等 DMA 排空，然后停止 I2S
    delay(200);
    i2s_stop(I2S_NUM_0);
    DEBUG_PRINTLN("[TTS] 测试音播放结束");
}
