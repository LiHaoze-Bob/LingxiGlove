#include "tts_player.h"
#include "config.h"
#include "http_client.h"
#include "llm_client.h"
#include "ws_server.h"   // 演示模式：i2s_write 同时广播 tts_audio 帧
#include <driver/i2s.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <math.h>

#if TTS_CACHE_ENABLE
#include <LittleFS.h>
#include <esp_partition.h>
#endif

static bool s_i2sInitialized = false;

// -----------------------------------------------------------------------
// PCM 累积缓冲（运行时从 PSRAM 动态申请，不占 DRAM .bss）
// -----------------------------------------------------------------------
// 必须放在 speak() 外部的两个原因：
//   1. 容量 192KB 无法放栈（loopTask 栈 8KB）；
//   2. speak() 内 static 变量两次调用之间内容不清零，若本次数据比上次短，
//      末尾会残留上次的旧 PCM → 播放顺序错乱（如"不，不用"变成"不用，不"）。
//   将其提到函数外 + 每次 speak() 入口强制清零前 12 字节（破坏旧 WAV 头），
//   彻底消除跨次调用的数据残留。
//
// 内存策略：
//   用 heap_caps_malloc(MALLOC_CAP_SPIRAM) 在运行时动态申请 PSRAM。
//   EXT_RAM_ATTR / EXT_RAM_BSS_ATTR 等编译期宏在 PSRAM 未开启时都是空宏，
//   会导致 DRAM .bss 溢出；heap_caps_malloc 更可靠——
//   PSRAM 可用时优先分 PSRAM，不可用时 fallback 到普通堆（此时 speak() 会
//   打印 WARN 但不崩溃，功能降级）。
// ⚠️ 前提：Arduino IDE → Tools → PSRAM → "OPI PSRAM" 必须开启，否则 fallback 到内部堆后 DRAM 会紧张。
static const size_t kPcmAccumBufSize = 192000;  // 约 4s @ 24kHz/16bit/Mono
static uint8_t* s_pcm_accum_buf = nullptr;       // initTTS() 里分配，生命周期覆盖整个运行期

// ======================================================================
// TTS 本地缓存（LittleFS）
// ======================================================================
#if TTS_CACHE_ENABLE

static bool s_cache_initialized = false;
static const char* kTtsCacheDir = "/tts_cache";

/**
 * @brief FNV-1a 32-bit 哈希：将任意长度 UTF-8 文本映射为 8 位 hex 文件名。
 *
 * 选择 FNV-1a 的理由：零依赖、常量时间复杂度、分布均匀，
 * 10 句演示词汇碰撞概率约 1e-8，完全可接受。
 */
static uint32_t Fnv1aHash(const char* str) {
    uint32_t hash = 0x811c9dc5u;  // FNV offset basis
    while (*str) {
        hash ^= (uint8_t)*str++;
        hash *= 0x01000193u;      // FNV prime
    }
    return hash;
}

/**
 * @brief 由文本生成缓存文件路径（如 "/tts_cache/a1b2c3d4.wav"）。
 *
 * @param text    输入文本（UTF-8）
 * @param out_path 输出路径缓冲区，至少 32 字节
 * @param out_size 缓冲区大小
 */
static void BuildCachePath(const char* text, char* out_path, size_t out_size) {
    uint32_t hash = Fnv1aHash(text);
    snprintf(out_path, out_size, "%s/%08x.wav", kTtsCacheDir, hash);
}

/**
 * @brief 从 LittleFS 缓存读取 WAV 到 PSRAM 缓冲区。
 *
 * @param cache_path  缓存文件路径
 * @param dst         目标缓冲区（PSRAM）
 * @param dst_size    缓冲区最大字节数
 * @return 读取的字节数；0 表示缓存不存在或读取失败
 */
static size_t ReadCache(const char* cache_path, uint8_t* dst, size_t dst_size) {
    if (!s_cache_initialized || dst == nullptr) return 0;

    File file = LittleFS.open(cache_path, "r");
    if (!file) return 0;

    size_t file_size = file.size();
    if (file_size == 0 || file_size > dst_size) {
        file.close();
        return 0;
    }

    size_t total_read = file.read(dst, file_size);
    file.close();
    return total_read;
}

/**
 * @brief 将 WAV 数据写入 LittleFS 缓存。
 *
 * 写入失败（Flash 满等）不影响正常 TTS 流程，仅打印 WARN。
 *
 * @param cache_path  缓存文件路径
 * @param src         WAV 数据源
 * @param src_len     数据字节数
 */
static void WriteCache(const char* cache_path, const uint8_t* src, size_t src_len) {
    if (!s_cache_initialized || src == nullptr || src_len == 0) return;

    File file = LittleFS.open(cache_path, "w");
    if (!file) {
        DEBUG_PRINTLN("[TTS缓存] 写入失败: 无法创建文件");
        return;
    }

    size_t written = file.write(src, src_len);
    file.close();

    if (written != src_len) {
        DEBUG_LOG("[TTS缓存] 写入不完整: %u/%u B", (uint32_t)written, (uint32_t)src_len);
        LittleFS.remove(cache_path);  // 删掉残缺文件，下次重新合成
    } else {
        DEBUG_LOG("[TTS缓存] 已缓存: %s (%u B)", cache_path, (uint32_t)src_len);
    }
}

#endif  // TTS_CACHE_ENABLE

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
// 工具函数：软件静音门控
// ----------------------------------------------------------------------
// 问题背景：
//   Qwen-TTS 在逗号/句号停顿段生成的 PCM 幅度从正常语音（±3000~±30000）
//   骤降到接近零（±10~±200）。MAX98357A 功放增益 9dB 会把这段量化底噪放大成
//   可听见的低频"嗷"变音（停顿处的特征噪声）。
//
// 原理：
//   扫描全量 int16 PCM 样本，若连续 kSilenceWindow 个样本的峰值幅度都低于
//   kSilenceThreshold，则视为停顿段，将该窗口内所有样本替换为严格零值。
//   替换为零后 MAX98357A 在 BCLK 持续时输出的是严格零信号，功放底噪消失。
//
// 参数选择：
//   kSilenceThreshold = 200：正常语音最低幅度约 800，量化噪底 < 100，阈值 200 居中。
//   kSilenceWindow    = 48 samples = 2ms @24kHz：足够短不影响语音起止，
//                       足够长不会误杀正常语音中的过零点。
//
// @param buf        int16 PCM 缓冲区首地址（字节）
// @param byte_count 缓冲区字节数（必须为偶数）
static void ApplySilenceGate(uint8_t* buf, size_t byte_count) {
    const int16_t kSilenceThreshold = 200;
    const size_t  kSilenceWindow    = 48;  // samples，约 2ms @24kHz

    int16_t* samples     = reinterpret_cast<int16_t*>(buf);
    size_t   sample_count = byte_count / sizeof(int16_t);

    size_t i = 0;
    while (i + kSilenceWindow <= sample_count) {
        // 找出窗口内峰值幅度
        int16_t peak = 0;
        for (size_t j = i; j < i + kSilenceWindow; ++j) {
            int16_t abs_val = samples[j] < 0 ? -samples[j] : samples[j];
            if (abs_val > peak) peak = abs_val;
        }
        if (peak < kSilenceThreshold) {
            // 峰值低于阈值：视为停顿段，替换为严格零值
            for (size_t j = i; j < i + kSilenceWindow; ++j) {
                samples[j] = 0;
            }
        }
        i += kSilenceWindow;
    }
    // 处理末尾不足一个窗口的样本（一般是语音结尾，幅度已趋零，也替换掉）
    for (; i < sample_count; ++i) {
        int16_t abs_val = samples[i] < 0 ? -samples[i] : samples[i];
        if (abs_val < kSilenceThreshold) samples[i] = 0;
    }
}

// ----------------------------------------------------------------------
// 工具函数：内存中的 RIFF WAV 头跳过
// ----------------------------------------------------------------------
// Qwen-TTS 非流式接口返回的 WAV 下载链接指向完整的 RIFF WAV 文件
// （"RIFF" + 长度 + "WAVE" + "fmt " chunk + "data" chunk header，约 44B）。
// 若不剥头直接送 I2S，前 ~44 字节会被当作 22 个 int16 PCM 样本播出 ——
// "RIFF\xAF\xFF\xFF\x7F" 这种字节会形成接近满刻度的方波 → 起始大爆音 +
// 整段听感全是杂音。
//
// 本函数操作下载到 PSRAM 的完整 WAV 内存 buffer，纯指针扫描，零 I/O 等待。
//
// @param buf 累积的 PCM/WAV 字节流首地址
// @param len buf 的总长度
// @return 若是 WAV 流，返回 data 段在 buf 中的起始偏移；
//         若不是 WAV（裸 PCM 或字节不足），返回 0
static size_t SkipWavHeader(const uint8_t* buf, size_t len) {
    if (buf == nullptr || len < 12) return 0;
    // 必须以 "RIFF"....+"WAVE" 开头才认为是 WAV
    if (buf[0] != 'R' || buf[1] != 'I' || buf[2] != 'F' || buf[3] != 'F' ||
        buf[8] != 'W' || buf[9] != 'A' || buf[10]!= 'V' || buf[11]!= 'E') {
        return 0;
    }
    // 顺序扫描 chunk header（id 4B + size 4B 小端），遇到 "data" 即返回其后偏移
    size_t pos = 12;
    const int kMaxChunks = 8;  // 防御异常 buffer 无限循环
    for (int i = 0; i < kMaxChunks && pos + 8 <= len; ++i) {
        const uint8_t* hdr = buf + pos;
        uint32_t size = (uint32_t)hdr[4]
                      | ((uint32_t)hdr[5] << 8)
                      | ((uint32_t)hdr[6] << 16)
                      | ((uint32_t)hdr[7] << 24);
        if (hdr[0] == 'd' && hdr[1] == 'a' && hdr[2] == 't' && hdr[3] == 'a') {
            // 找到 data chunk，返回其负载起始偏移
            return pos + 8;
        }
        // 流式 WAV 的 fmt/LIST 等 chunk size 字段可能填占位的超大值；
        // 一旦发现 size 越界，按"无法继续解析"放弃跳过（当裸 PCM 处理）。
        if (size > len || pos + 8 + size > len) {
            return 0;
        }
        pos += 8 + size;
    }
    return 0;  // 8 个 chunk 内未找到 data，放弃
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

    // 分配 PCM 累积缓冲到 PSRAM（192KB，占满内部 DRAM .bss 会链接报错）
    // heap_caps_malloc 比 EXT_RAM_ATTR 更可靠：后者在 PSRAM 未配置时是空宏会回落 DRAM，
    // 而 heap_caps_malloc(MALLOC_CAP_SPIRAM) 在 PSRAM 不可用时会返回 nullptr（可检测）。
    if (s_pcm_accum_buf == nullptr) {
        s_pcm_accum_buf = (uint8_t*)heap_caps_malloc(kPcmAccumBufSize, MALLOC_CAP_SPIRAM);
        if (s_pcm_accum_buf == nullptr) {
            // PSRAM 不可用时 fallback 到内部堆（可能导致 DRAM 紧张，仅作兜底）
            DEBUG_PRINTLN("[TTS] 警告: PSRAM 分配失败，尝试内部堆（可能内存不足）");
            s_pcm_accum_buf = (uint8_t*)malloc(kPcmAccumBufSize);
        }
        if (s_pcm_accum_buf == nullptr) {
            DEBUG_PRINTLN("[TTS] 错误: PCM 缓冲分配彻底失败，TTS 不可用");
            i2s_driver_uninstall(I2S_NUM_0);
            return false;
        }
        DEBUG_LOG("[TTS] PCM 累积缓冲已分配: %u B @ 0x%08X",
                  (uint32_t)kPcmAccumBufSize, (uint32_t)(uintptr_t)s_pcm_accum_buf);
    }

    s_i2sInitialized = true;
    DEBUG_PRINTLN("[TTS] I2S初始化成功 (24kHz, 16bit, Mono)");

#if TTS_CACHE_ENABLE
    // LittleFS 初始化：formatOnFail=true 首次自动格式化 Flash 分区
    if (!s_cache_initialized) {
        if (LittleFS.begin(true)) {
            // 确保缓存目录存在
            if (!LittleFS.exists(kTtsCacheDir)) {
                LittleFS.mkdir(kTtsCacheDir);
            }
            s_cache_initialized = true;
            DEBUG_LOG("[TTS缓存] LittleFS 初始化成功, 总容量=%u B, 已用=%u B",
                      (uint32_t)LittleFS.totalBytes(), (uint32_t)LittleFS.usedBytes());
        } else {
            DEBUG_PRINTLN("[TTS缓存] LittleFS 初始化失败，缓存功能不可用");
            // 用 ESP-IDF 分区表 API 诊断 Flash 上实际的分区布局
            const esp_partition_t* spiffs_part =
                esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                        ESP_PARTITION_SUBTYPE_DATA_SPIFFS, nullptr);
            if (spiffs_part != nullptr) {
                DEBUG_LOG("[TTS缓存] Flash 上存在 spiffs 分区: label='%s' offset=0x%X size=%u B (%u KB)",
                          spiffs_part->label,
                          (uint32_t)spiffs_part->address,
                          (uint32_t)spiffs_part->size,
                          (uint32_t)(spiffs_part->size / 1024));
                DEBUG_PRINTLN("[TTS缓存] spiffs 分区存在但 LittleFS 挂载失败");
                DEBUG_PRINTLN("[TTS缓存]   可能原因: 分区未格式化（formatOnFail 未生效）");
                DEBUG_PRINTLN("[TTS缓存]   尝试: 用 esptool erase_flash 擦除后重新烧录");
            } else {
                DEBUG_PRINTLN("[TTS缓存] *** Flash 上没有 spiffs 分区 ***");
                DEBUG_PRINTLN("[TTS缓存] 诊断: Arduino Nano ESP32-S3 默认用 dfu-util 上传");
                DEBUG_PRINTLN("[TTS缓存]   dfu-util 只烧应用固件，不烧分区表！");
                DEBUG_PRINTLN("[TTS缓存]   即使 IDE 选了 'With SPIFFS partition'，分区表也没写入 Flash");
                DEBUG_PRINTLN("[TTS缓存] 解决: 需用 esptool 手动烧录一次 bootloader + 分区表");
                DEBUG_PRINTLN("[TTS缓存]   步骤1: Arduino IDE → Sketch → Export Compiled Binary");
                DEBUG_PRINTLN("[TTS缓存]   步骤2: 双击 RST 按钮让板子进入 bootloader 模式");
                DEBUG_PRINTLN("[TTS缓存]   步骤3: 用 esptool.py write_flash 烧录 .partitions.bin 到 0x8000");
            }
            // 列出 Flash 上所有 data 分区，辅助诊断
            esp_partition_iterator_t iter = esp_partition_find(
                ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, nullptr);
            if (iter != nullptr) {
                DEBUG_PRINTLN("[TTS缓存] Flash 上的 data 分区列表:");
                while (iter != nullptr) {
                    const esp_partition_t* part = esp_partition_get(iter);
                    DEBUG_LOG("[TTS缓存]   label='%s' subtype=0x%02X offset=0x%X size=%u KB",
                              part->label, part->subtype,
                              (uint32_t)part->address,
                              (uint32_t)(part->size / 1024));
                    iter = esp_partition_next(iter);
                }
                esp_partition_iterator_release(iter);
            }
        }
    }
#endif

    return true;
}


/**
 * @brief 云端 TTS：通过阿里 DashScope Qwen-TTS 非流式合成并播报。
 *
 * 流程：POST 请求合成 → 服务端返回 WAV 下载 URL → GET 下载到 PSRAM → 播放。
 *
 * 非流式模式的优势（相比旧 SSE 流式方案）：
 *   - 服务端返回完整 WAV 文件 URL，GET 下载即可，无需 base64 分帧解码
 *   - 彻底消除帧拼接、字节对齐、PSRAM cache 逐字节写入等问题
 *   - 代码大幅简化（~160 行 SSE 回调 + Base64Decode → 0）
 *   - 代价：延迟略高（需等服务端合成完），但短句（≤10 字）差异不明显
 *
 * @param text      要朗读的文本，非空且长度 <= 600 字符（Qwen-TTS 上限）
 * @param cache_key  缓存索引键（可选）。非空时用此键查找/写入缓存，
 *                   解决 LLM 改写不稳定导致缓存永远无法命中的问题。
 *                   为 nullptr 时退化为以 text 本身作为缓存键。
 * @param cache_only true 时仅查本地缓存，命中则播放并返回 true，
 *                   未命中直接返回 false（不走云端合成）。
 *                   典型用法：在 LLM 改写之前先尝试缓存播放，
 *                   命中则跳过 LLM + 云端 TTS，延迟从 3-5s 降至 <100ms。
 * @return true 播放完成；false 任一环节失败（调用方可回落到 local_tts_fallback）
 */
bool speak(const char* text, const char* cache_key, bool cache_only) {
    if (!s_i2sInitialized) {
        DEBUG_PRINTLN("[TTS] 错误: I2S 未初始化");
        return false;
    }
    if (s_pcm_accum_buf == nullptr) {
        DEBUG_PRINTLN("[TTS] 错误: PCM 缓冲未分配（PSRAM 不可用），TTS 跳过");
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

    // -------- 缓存查找 --------
    // cache_key 非空时以其为索引（典型场景：传入原始手势词，避免 LLM 改写
    // 不稳定导致同一手势每次哈希不同、永远无法命中缓存）。
    // cache_key 为 nullptr 时退化为以 text 本身为索引（兼容手动 TTS 等场景）。
    size_t accum_len = 0;
    bool from_cache = false;

#if TTS_CACHE_ENABLE
    const char* effective_key = (cache_key != nullptr && cache_key[0] != '\0')
                                ? cache_key : text;
    char cache_path[32];
    BuildCachePath(effective_key, cache_path, sizeof(cache_path));

    accum_len = ReadCache(cache_path, s_pcm_accum_buf, kPcmAccumBufSize);
    if (accum_len > 0) {
        from_cache = true;
        DEBUG_LOG("[TTS] 缓存命中: %s key='%s' (%u B)",
                  cache_path, effective_key, (uint32_t)accum_len);
    }
#endif

    // -------- 缓存未命中 --------
    if (!from_cache) {
        // cache_only 模式：仅查缓存，未命中时不走云端，直接返回 false。
        // 调用方可据此决定是否跳过 LLM 改写（缓存命中 → 跳过 LLM，节省 1-2s）。
        if (cache_only) {
            return false;
        }
        if (!WiFi.isConnected()) {
            DEBUG_PRINTLN("[TTS] 错误: WiFi 未连接且缓存未命中");
            return false;
        }

        // 构建非流式 POST 请求
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

        DEBUG_LOG("[TTS] 请求合成(非流式): %s", text);

        // 第一步：POST 获取 WAV 下载 URL
        String response = httpPostJson(QWEN_TTS_ENDPOINT, payload,
                                       auth_header.c_str(), 30000);
        if (response.length() == 0) {
            DEBUG_PRINTLN("[TTS] 错误: 合成请求失败（空响应）");
            return false;
        }

        JsonDocument resp_doc;
        DeserializationError json_err = deserializeJson(resp_doc, response);
        if (json_err) {
            DEBUG_LOG("[TTS] 错误: JSON 解析失败: %s", json_err.c_str());
            return false;
        }

        const char* audio_url = resp_doc["output"]["audio"]["url"];
        if (audio_url == nullptr || audio_url[0] == '\0') {
            const char* err_code = resp_doc["code"];
            const char* err_msg  = resp_doc["message"];
            if (err_code) {
                DEBUG_LOG("[TTS] 服务端错误: code=%s msg=%s",
                          err_code, err_msg ? err_msg : "(null)");
            } else {
                DEBUG_PRINTLN("[TTS] 错误: 响应中无 audio.url");
                char preview[201] = {0};
                size_t copy_len = response.length() < 200 ? response.length() : 200;
                memcpy(preview, response.c_str(), copy_len);
                DEBUG_LOG("[TTS] 响应预览: %s", preview);
            }
            return false;
        }

        DEBUG_LOG("[TTS] 获取到音频 URL: %.80s...", audio_url);

        // 第二步：GET 下载 WAV 到 PSRAM 缓冲
        accum_len = httpGetToBuffer(audio_url, s_pcm_accum_buf, kPcmAccumBufSize);
        if (accum_len == 0) {
            DEBUG_PRINTLN("[TTS] 错误: WAV 下载失败");
            return false;
        }
        DEBUG_LOG("[TTS] WAV 下载完成: %u B", (uint32_t)accum_len);

        // 写入缓存供下次使用
#if TTS_CACHE_ENABLE
        WriteCache(cache_path, s_pcm_accum_buf, accum_len);
#endif
    }

    // -------- 预切 I2S 采样率到 24kHz --------
    bool rate_switched = false;
    if (QWEN_TTS_SAMPLE_RATE != TTS_I2S_DEFAULT_SAMPLE_RATE) {
        i2s_zero_dma_buffer(I2S_NUM_0);
        esp_err_t err = i2s_set_sample_rates(I2S_NUM_0, QWEN_TTS_SAMPLE_RATE);
        if (err != ESP_OK) {
            DEBUG_LOG("[TTS] 错误: 预切采样率失败 err=%d", (int)err);
            return false;
        }
        rate_switched = true;
    }

    // -------- 写 I2S 播放 --------
    bool got_any_audio = false;
    size_t total_written = 0;

    if (accum_len > 0) {
        // 剥掉 RIFF WAV 头（标准 44 字节）
        const size_t pcm_offset = SkipWavHeader(s_pcm_accum_buf, accum_len);
        const size_t pcm_len    = accum_len - pcm_offset;
        if (pcm_offset > 0) {
            DEBUG_LOG("[TTS] 检测到 WAV 头，跳过前 %u 字节，有效 PCM=%u B",
                      (uint32_t)pcm_offset, (uint32_t)pcm_len);
        }
        uint8_t* pcm_buf = s_pcm_accum_buf + pcm_offset;

        // 软件增益（gain=1.0 时编译器优化为空）
        ApplyGain(pcm_buf, pcm_len);

        // 软件静音门控：消除停顿段"嗷"变音
        //   Qwen-TTS 在逗号/句号停顿处生成的 PCM 幅度骤降到接近零（±10~±200），
        //   MAX98357A 9dB 增益放大量化底噪 → 可听见的低频"嗷"变音。
        //   门控把低于阈值的连续低幅度段替换为严格零值，功放底噪消失。
        ApplySilenceGate(pcm_buf, pcm_len);

        // 启动 I2S 时钟，再清零 DMA 缓冲：
        //   i2s_zero_dma_buffer() 必须在 I2S running 状态下调用才能真正写零。
        //   若在 i2s_stop() 后调用，DMA 描述符已挂起，清零不生效；上次播放
        //   末尾残留的非零样本会在 i2s_start() 后的第一个 DMA 周期立即输出
        //   → 听到播放前一声短促杂音。
        //   正确顺序：先 start（BCLK 恢复）→ 再 zero_dma（清掉残留）→ 再写静音预热。
        i2s_start(I2S_NUM_0);
        i2s_zero_dma_buffer(I2S_NUM_0);

        // MAX98357A 冷启动静音预热：
        //   功放从 BCLK 停止 → BCLK 启动时内部放大器需要约 100-150ms 稳定，
        //   这段时间直接播 PCM 会把第一个字的声母（高频能量）叠加在冷启动
        //   冲击噪声上，听感是首字被"噗"声覆盖或音色严重失真。
        //   写入 100ms 零值静音帧让功放充分预热后，再送真实 PCM。
        //   kWarmupBytes 必须是 4 的倍数（I2S DMA 描述符对齐要求）。
        {
            const size_t kWarmupBytes = (24000u * 2u * 100u) / 1000u;  // 100ms @ 24kHz/16bit
            uint8_t warmup_chunk[256];
            memset(warmup_chunk, 0, sizeof(warmup_chunk));
            size_t warmup_remaining = kWarmupBytes;
            while (warmup_remaining > 0) {
                size_t once = warmup_remaining < sizeof(warmup_chunk)
                              ? warmup_remaining : sizeof(warmup_chunk);
                size_t written = 0;
                i2s_write(I2S_NUM_0, warmup_chunk, once, &written, portMAX_DELAY);
                total_written += written;
                warmup_remaining -= once;
            }
        }

        // 分块写 I2S，每块 4096 字节对齐 DMA 描述符大小
        // 演示模式钩子：每块 i2s_write 后同步广播一帧 tts_audio，
        // base64+JSON+WS broadcastTXT 通常 < 20ms，DMA 缓冲足以掩盖延迟。
        const bool demo_mode = WsServer_IsDemoMode();
        uint32_t tts_seq = 0;
        const size_t kI2SChunk = 4096;
        size_t offset = 0;
        while (offset < pcm_len) {
            size_t chunk = pcm_len - offset;
            if (chunk > kI2SChunk) chunk = kI2SChunk;
            size_t bytes_written = 0;
            i2s_write(I2S_NUM_0, pcm_buf + offset, chunk,
                      &bytes_written, portMAX_DELAY);
            if (demo_mode) {
                // pcm_buf 是 16-bit LE，chunk 字节数对齐 2
                WsServer_BroadcastTtsAudio(
                    reinterpret_cast<const int16_t*>(pcm_buf + offset),
                    chunk / sizeof(int16_t),
                    tts_seq,
                    /*final=*/false,
                    tts_seq == 0 ? text : nullptr,  // 仅首帧带原文
                    QWEN_TTS_SAMPLE_RATE);
                tts_seq++;
            }
            total_written += bytes_written;
            offset += chunk;
        }
        // 演示模式收尾帧：通知 APP 释放 AudioContext 队列
        if (demo_mode) {
            WsServer_BroadcastTtsAudio(nullptr, 0, tts_seq, /*final=*/true,
                                       nullptr, QWEN_TTS_SAMPLE_RATE);
        }
        got_any_audio = true;
        DEBUG_LOG("[TTS] I2S 写入完成, %u bytes (含 100ms 预热)", (uint32_t)total_written);

        // 精确等待 DMA 排空后立即 stop：
        //
        // 【修复：拖音/杂音根因】
        // i2s_write() 是把数据 enqueue 到 DMA 队列，返回时数据尚未输出完。
        // 需要等待 total_written 字节全部从 DMA 输出到功放，然后立即 i2s_stop()。
        //
        // 旧方案取 max(play_ms, dma_drain=682ms)，对短词（如"不。"total_written=383ms）
        // 等了 782ms，I2S 在语音结束后还多运行 ~400ms 输出零值——MAX98357A 在 BCLK
        // 持续但信号为零时功放偏置噪声持续输出，听感即为"拖音/杂音"。
        //
        // 新方案：wait_ms = total_written 对应的播放时长 + 50ms 安全余量
        //   total_written 已是 DMA 里所有待输出数据（warmup + PCM）的字节数，
        //   等这么久后数据恰好全部输出完，立即 stop 可消除拖音。
        uint32_t play_ms = (uint32_t)((total_written * 1000UL) / (QWEN_TTS_SAMPLE_RATE * 2));
        uint32_t wait_ms = play_ms + 50u;  // 50ms 安全余量防末尾样本被截
        DEBUG_LOG("[TTS] 等待 DMA 排空 %u ms (写入 %u ms + 余量 50ms)",
                  wait_ms, play_ms);
        delay(wait_ms);

        // 停止 I2S 时钟：让 MAX98357A 自动进入静音状态，消除结尾拖音。
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

    DEBUG_LOG("[TTS] 播放完成, WAV=%u B, 写入=%u bytes, 收到音频=%s",
              (uint32_t)accum_len, (uint32_t)total_written, got_any_audio ? "是" : "否");

    return got_any_audio;
}

bool PlayPcmInt16(const int16_t* pcm, size_t sample_count, uint32_t sample_rate,
                  const char* label) {
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
    const bool demo_mode = WsServer_IsDemoMode();
    uint32_t tts_seq = 0;
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
        // 演示模式钩子：与本地 I2S 同步广播 tts_audio
        if (demo_mode) {
            WsServer_BroadcastTtsAudio(
                gain_buf, chunk, tts_seq, /*final=*/false,
                tts_seq == 0 ? label : nullptr,
                sample_rate);
            tts_seq++;
        }
        written_samples += bytes_written / sizeof(int16_t);
        // 让出给看门狗
        if (written_samples % (kChunkSamples * 4) == 0) {
            delay(1);
        }
    }
    // 演示模式收尾帧
    if (demo_mode) {
        WsServer_BroadcastTtsAudio(nullptr, 0, tts_seq, /*final=*/true,
                                   nullptr, sample_rate);
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

// ======================================================================
// TTS 缓存管理接口
// ======================================================================

void clearTtsCache() {
#if TTS_CACHE_ENABLE
    if (!s_cache_initialized) {
        DEBUG_PRINTLN("[TTS缓存] LittleFS 未初始化，无法清除");
        return;
    }

    File dir = LittleFS.open(kTtsCacheDir);
    if (!dir || !dir.isDirectory()) {
        DEBUG_PRINTLN("[TTS缓存] 缓存目录不存在，无需清除");
        return;
    }

    int removed_count = 0;
    File entry = dir.openNextFile();
    while (entry) {
        String path = String(kTtsCacheDir) + "/" + entry.name();
        entry.close();
        LittleFS.remove(path);
        removed_count++;
        entry = dir.openNextFile();
    }
    dir.close();

    DEBUG_LOG("[TTS缓存] 已清除 %d 个缓存文件", removed_count);
#else
    DEBUG_PRINTLN("[TTS缓存] 缓存功能未启用 (TTS_CACHE_ENABLE=0)");
#endif
}
