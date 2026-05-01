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
            *out_data_bytes = size;
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
        .sample_rate = 16000,
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
    DEBUG_PRINTLN("[TTS] I2S初始化成功 (16kHz, 16bit, Mono)");
    return true;
}

/**
 * @brief 云端 TTS：通过阿里 DashScope Qwen-TTS 合成并播报。
 *
 * 工程流程（与百度 TTS 的关键差异：Qwen-TTS 是"两步 HTTP"而非单次 GET）：
 *   步骤 1 —— POST 合成请求到 QWEN_TTS_ENDPOINT，体为 JSON，返回 JSON
 *             里的 output.audio.url 是一个 24h 有效的 .wav 文件下载地址；
 *   步骤 2 —— GET 该 url，解析 44B RIFF/WAVE 头取出采样率与 data 段长度，
 *             剩余字节即 int16 PCM 数据，流式写入 I2S 播放；
 *   步骤 3 —— 播放结束后恢复 I2S 默认采样率（16 kHz），避免影响其它播放路径。
 *
 * 内存约束：
 *   Qwen-TTS 以 24 kHz/16-bit/Mono 输出，若整段缓存 480 KB 会耗尽 ESP32-S3 SRAM。
 *   因此本函数**全程流式**处理，仅保留 1 KB 本地栈缓冲，不使用 String 积累音频。
 *
 * 超时策略（沿用百度版本的经验参数，已在 MVP 阶段验证过）：
 *   - 元数据阶段（POST 拿 url）：HTTPClient 自身 timeout；
 *   - 下载+播放阶段：单次最长 15 s，连续 3 s 无数据即中止，避免网络半关死等。
 *
 * @param text 要朗读的文本，非空且长度 <= 600 字符（Qwen-TTS 上限为 512 Token / 600 字符）
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
    // Qwen-TTS 官方限制 600 字符；超过会被截断或返回 400，不如端侧直接拒绝
    const size_t kMaxTextBytes = 600 * 3;  // 中文 UTF-8 最多 3 字节/字
    size_t text_len = strlen(text);
    if (text_len > kMaxTextBytes) {
        DEBUG_PRINT("[TTS] 错误: 文本过长 (bytes=");
        DEBUG_PRINT((uint32_t)text_len);
        DEBUG_PRINTLN(")");
        return false;
    }
    if (!WiFi.isConnected()) {
        DEBUG_PRINTLN("[TTS] 错误: WiFi 未连接");
        return false;
    }

    // -------- 步骤 1：POST 合成请求，解析 audio.url --------
    String audio_url;
    {
        // ArduinoJson 7.x：JsonDocument 不再是模板，内部按需分配堆内存。
        // 本函数属低频路径（每次播报一次），堆碎片风险可接受。
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

        DEBUG_PRINT("[TTS] 请求合成: ");
        DEBUG_PRINTLN(text);

        String response = httpPostJson(QWEN_TTS_ENDPOINT, payload, auth_header.c_str());
        if (response.length() == 0) {
            DEBUG_PRINTLN("[TTS] 错误: Qwen-TTS POST 无响应");
            return false;
        }

        // 响应 JSON 主要是元数据 + url（典型 < 2 KB），JsonDocument 足够
        JsonDocument resp_doc;
        DeserializationError err = deserializeJson(resp_doc, response);
        if (err) {
            DEBUG_PRINT("[TTS] 错误: 响应 JSON 解析失败: ");
            DEBUG_PRINTLN(err.c_str());
            return false;
        }
        const char* url_raw = resp_doc["output"]["audio"]["url"];
        if (url_raw == nullptr || url_raw[0] == '\0') {
            // 尝试打印错误字段，方便排查鉴权/文本格式问题
            const char* api_code = resp_doc["code"];
            const char* api_msg  = resp_doc["message"];
            DEBUG_PRINT("[TTS] 错误: 响应缺 output.audio.url; code=");
            DEBUG_PRINT(api_code ? api_code : "(null)");
            DEBUG_PRINT(" message=");
            DEBUG_PRINTLN(api_msg  ? api_msg  : "(null)");
            return false;
        }
        audio_url = url_raw;
    }

    DEBUG_PRINT("[TTS] 获取到音频 URL, 长度=");
    DEBUG_PRINTLN(audio_url.length());

    // -------- 步骤 2：GET 音频流，解析 WAV 头 --------
    HTTPClient http;
    http.setTimeout(10000);  // 下载建连阶段 10s
    if (!http.begin(audio_url)) {
        DEBUG_PRINTLN("[TTS] 错误: 无法建立音频下载连接");
        return false;
    }

    int http_code = http.GET();
    if (http_code != HTTP_CODE_OK) {
        DEBUG_PRINT("[TTS] 错误: 音频 URL 返回 HTTP ");
        DEBUG_PRINTLN(http_code);
        http.end();
        return false;
    }

    WiFiClient* stream = http.getStreamPtr();
    if (stream == nullptr) {
        DEBUG_PRINTLN("[TTS] 错误: 无法获取音频流");
        http.end();
        return false;
    }

    uint32_t wav_rate = 0;
    uint16_t wav_ch = 0;
    uint16_t wav_bps = 0;
    uint32_t wav_data_bytes = 0;
    if (!ReadWavHeader(stream, &wav_rate, &wav_ch, &wav_bps, &wav_data_bytes)) {
        DEBUG_PRINTLN("[TTS] 错误: WAV 头解析失败");
        http.end();
        return false;
    }
    // I2S 在 initTTS 里是 16-bit / Mono 配置；WAV 必须匹配这两项，否则拒绝播放
    if (wav_ch != 1 || wav_bps != 16) {
        DEBUG_PRINT("[TTS] 错误: WAV 格式不兼容 ch=");
        DEBUG_PRINT(wav_ch);
        DEBUG_PRINT(" bps=");
        DEBUG_PRINTLN(wav_bps);
        http.end();
        return false;
    }
    if (wav_rate < 8000u || wav_rate > 48000u) {
        DEBUG_PRINT("[TTS] 错误: WAV 采样率越界: ");
        DEBUG_PRINTLN(wav_rate);
        http.end();
        return false;
    }

    DEBUG_PRINT("[TTS] WAV 就绪 rate=");
    DEBUG_PRINT(wav_rate);
    DEBUG_PRINT(" Hz, data=");
    DEBUG_PRINT(wav_data_bytes);
    DEBUG_PRINTLN(" bytes");

    // -------- 步骤 3：切采样率 → 流式写 I2S → 恢复 --------
    bool rate_switched = false;
    if (wav_rate != TTS_I2S_DEFAULT_SAMPLE_RATE) {
        esp_err_t err = i2s_set_sample_rates(I2S_NUM_0, wav_rate);
        if (err != ESP_OK) {
            DEBUG_PRINT("[TTS] 错误: i2s_set_sample_rates 失败 err=");
            DEBUG_PRINTLN((int)err);
            http.end();
            return false;
        }
        rate_switched = true;
    }

    // 网络读缓冲尺寸：
    //   4 KB 对应 24 kHz/16-bit/Mono 下约 85 ms 音频；结合 DMA 680 ms 深度，
    //   单轮 readBytes 只要能在 600 ms 内返回，DMA 就不会饥饿。
    //   经验值：WiFi 正常时单次 TCP 包 1.4 KB，4 KB 缓冲可一次容纳 2-3 个包，
    //   比原先 1 KB（仅容纳 1 个包，且可能截断）显著提高吞吐与抗抖性。
    //
    // 重要：必须使用 static 放入 .bss 段，不能放栈上。
    //   Arduino Nano ESP32 (NORA-W106) 的 loopTask 栈默认仅 8 KB；
    //   叠加 WiFiClientSecure / HTTPClient / mbedtls 调用链，栈已非常紧张，
    //   若再在栈上开 4 KB 缓冲会触发 "Stack canary watchpoint triggered"
    //   导致复位（表现为执行到 POST 立即重启，日志戛然而止）。
    //   speak() 不会被并发/重入调用（串口/手势均为串行触发），static 安全。
    static uint8_t s_download_buf[4096];
    size_t  total_written = 0;
    size_t  remaining     = (size_t)wav_data_bytes;

    const unsigned long kOverallTimeoutMs = 15000;  // 单次播报最长 15 s
    const unsigned long kIdleTimeoutMs    = 3000;   // 连续 3 s 无数据中止
    const unsigned long start_ms = millis();
    unsigned long last_data_ms = start_ms;

    while (remaining > 0 && (http.connected() || stream->available() > 0)) {
        size_t want = (remaining > sizeof(s_download_buf)) ? sizeof(s_download_buf) : remaining;
        int available = stream->available();
        if (available > 0) {
            // 尽量按 want 读满；readBytes 内部会等到 want 或超时才返回，
            // 比之前按 available 单包读效率更高、I2S 喂数据更连续。
            size_t got = stream->readBytes(s_download_buf, want);
            if (got > 0) {
                size_t bytes_written = 0;
                i2s_write(I2S_NUM_0, s_download_buf, got, &bytes_written, portMAX_DELAY);
                total_written += bytes_written;
                remaining     -= got;
                last_data_ms  = millis();
            }
        } else {
            if (millis() - last_data_ms > kIdleTimeoutMs) {
                DEBUG_PRINTLN("[TTS] 警告: 音频流空读超时，提前结束");
                break;
            }
            // 空等间隔由 2 ms 缩到 1 ms：WiFi 包到达本就是 ms 级事件，更密的轮询
            // 能缩短"数据已到但我们还没读"的盲区，配合大 DMA 缓冲基本消除顿挫。
            delay(1);
        }
        if (millis() - start_ms > kOverallTimeoutMs) {
            DEBUG_PRINTLN("[TTS] 警告: 音频总时长超限，提前结束");
            break;
        }
    }

    // 等 DMA 排空，避免末尾截断
    delay(200);

    // 恢复默认采样率（与 PlayPcmInt16 一致的收尾约定）
    if (rate_switched) {
        esp_err_t err = i2s_set_sample_rates(I2S_NUM_0, TTS_I2S_DEFAULT_SAMPLE_RATE);
        if (err != ESP_OK) {
            // 不影响本次播放成败；仅记录，后续 speak 时 initTTS 的默认率仍是 16k
            DEBUG_PRINT("[TTS] 警告: 恢复默认采样率失败 err=");
            DEBUG_PRINTLN((int)err);
        }
    }

    http.end();

    DEBUG_PRINT("[TTS] 播放完成, 共写入 ");
    DEBUG_PRINT((uint32_t)total_written);
    DEBUG_PRINT(" bytes / 预期 ");
    DEBUG_PRINT(wav_data_bytes);
    DEBUG_PRINTLN(" bytes");

    // 写入量 < 50% 视为失败，允许上层回落到离线兜底
    if (total_written * 2 < (size_t)wav_data_bytes) {
        return false;
    }
    return true;
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
        DEBUG_PRINT("[TTS] PlayPcmInt16: sample_rate 越界: ");
        DEBUG_PRINTLN(sample_rate);
        return false;
    }
    // 最长播放 10 秒，避免传入巨大数组阻塞主循环过久
    const size_t kMaxSamples = (size_t)sample_rate * 10u;
    if (sample_count > kMaxSamples) {
        DEBUG_PRINT("[TTS] PlayPcmInt16: sample_count 超过 10 秒上限: ");
        DEBUG_PRINTLN((uint32_t)sample_count);
        return false;
    }

    // 如采样率与默认不同，临时切换
    bool rate_switched = false;
    if (sample_rate != TTS_I2S_DEFAULT_SAMPLE_RATE) {
        esp_err_t err = i2s_set_sample_rates(I2S_NUM_0, sample_rate);
        if (err != ESP_OK) {
            DEBUG_PRINT("[TTS] PlayPcmInt16: i2s_set_sample_rates 失败 err=");
            DEBUG_PRINTLN((int)err);
            return false;
        }
        rate_switched = true;
    }

    // 分块写 I2S（一次写太大会阻塞任务切换；每块 512 样本 = 1024 字节对齐 DMA buf）
    const size_t kChunkSamples = 512;
    size_t written_samples = 0;
    while (written_samples < sample_count) {
        size_t chunk = sample_count - written_samples;
        if (chunk > kChunkSamples) chunk = kChunkSamples;
        size_t bytes_written = 0;
        esp_err_t err = i2s_write(I2S_NUM_0,
                                  pcm + written_samples,
                                  chunk * sizeof(int16_t),
                                  &bytes_written,
                                  portMAX_DELAY);
        if (err != ESP_OK) {
            DEBUG_PRINT("[TTS] PlayPcmInt16: i2s_write 失败 err=");
            DEBUG_PRINTLN((int)err);
            if (rate_switched) {
                i2s_set_sample_rates(I2S_NUM_0, TTS_I2S_DEFAULT_SAMPLE_RATE);
            }
            return false;
        }
        written_samples += bytes_written / sizeof(int16_t);
        // 让出给看门狗
        if (written_samples % (kChunkSamples * 4) == 0) {
            delay(1);
        }
    }

    // 等 DMA 排空
    delay(100);

    // 恢复默认采样率，避免影响后续 speak()
    if (rate_switched) {
        i2s_set_sample_rates(I2S_NUM_0, TTS_I2S_DEFAULT_SAMPLE_RATE);
    }
    return true;
}

void playTestTone(int freq, int durationMs) {
    if (!s_i2sInitialized) return;

    // 频率/时长边界保护：防止除零或参数异常导致的死循环
    if (freq < 50 || freq > 8000 || durationMs <= 0) {
        DEBUG_PRINTLN("[TTS] 测试音参数非法 (freq 应在 50~8000Hz)");
        return;
    }

    DEBUG_PRINT("[TTS] 播放测试音 ");
    DEBUG_PRINT(freq);
    DEBUG_PRINT("Hz, 持续 ");
    DEBUG_PRINT(durationMs);
    DEBUG_PRINTLN("ms");

    const int sampleRate = 16000;
    const int samplesPerPeriod = sampleRate / freq;
    if (samplesPerPeriod < 2) {
        DEBUG_PRINTLN("[TTS] 频率过高，无法生成测试音");
        return;
    }

    int16_t sample;
    size_t bytesWritten = 0;
    unsigned long startTime = millis();

    while (millis() - startTime < (unsigned long)durationMs) {
        for (int i = 0; i < samplesPerPeriod; i++) {
            sample = (int16_t)(sin(2.0 * PI * i / samplesPerPeriod) * 2000);
            i2s_write(I2S_NUM_0, &sample, sizeof(sample), &bytesWritten, portMAX_DELAY);
        }
    }

    delay(200);  // 等待音频排空
    DEBUG_PRINTLN("[TTS] 测试音播放结束");
}
