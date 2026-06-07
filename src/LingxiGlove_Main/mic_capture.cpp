// ============================================================
// mic_capture.cpp
// 见 mic_capture.h 头部说明
//
// 当前阶段：ENABLE_MIC_CAPTURE = 0 时所有公开函数走 stub 分支，不会
//           引入 driver/i2s 依赖；当物理麦克风到位后翻开开关，
//           "TODO(mic-capture):" 标记的位置补全 ESP-IDF I2S 调用即可。
// ============================================================
#include "mic_capture.h"

#if ENABLE_MIC_CAPTURE
#include <Arduino.h>
#include "driver/i2s.h"
#endif

#include <math.h>
#include <string.h>

namespace {

bool  s_initialized = false;
bool  s_running     = false;
float s_level       = 0.0f;  // 0~1，由 ReadChunk 内的 RMS 计算并平滑

float ComputeRms16(const int16_t* pcm, size_t n) {
    if (!pcm || n == 0) return 0.0f;
    double acc = 0.0;
    for (size_t i = 0; i < n; i++) {
        const float v = (float)pcm[i] / 32768.0f;  // 归一化 -1~1
        acc += (double)(v * v);
    }
    return (float)sqrt(acc / (double)n);
}

}  // namespace

bool MicCapture_Init() {
#if !ENABLE_MIC_CAPTURE
    s_initialized = true;  // stub 模式：声称成功，便于上层无感知接入
    return true;
#else
    if (s_initialized) return true;

    i2s_config_t cfg = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
        .sample_rate = MIC_SAMPLE_RATE_HZ,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,  // INMP441 实测必须 32bit 帧
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = MIC_DMA_BUF_COUNT,
        .dma_buf_len   = MIC_DMA_BUF_SAMPLES,
        .use_apll = false,
        .tx_desc_auto_clear = false,
        .fixed_mclk = 0
    };
    i2s_pin_config_t pins = {
        // i2s_set_pin 是 ESP-IDF API，不参与 Arduino 框架的 pin remap。在
        // Nano ESP32 "By Arduino pin (default)" 模式下，Dx 是抽象 pin 号
        // (D10=10/D11=11/D12=12)，必须 digitalPinToGPIONumber() 转换成
        // 真实 GPIO (21/38/47)，否则 I2S 信号会驱动到错误的物理脚。
        .bck_io_num   = digitalPinToGPIONumber(MIC_I2S_BCLK),
        .ws_io_num    = digitalPinToGPIONumber(MIC_I2S_LRCLK),
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num  = digitalPinToGPIONumber(MIC_I2S_DIN)
    };

    if (i2s_driver_install((i2s_port_t)MIC_I2S_PORT_NUM, &cfg, 0, NULL) != ESP_OK) {
        return false;
    }
    if (i2s_set_pin((i2s_port_t)MIC_I2S_PORT_NUM, &pins) != ESP_OK) {
        i2s_driver_uninstall((i2s_port_t)MIC_I2S_PORT_NUM);
        return false;
    }
    // INMP441 冷启动约 100ms，清空 DMA 内初始噪声
    i2s_zero_dma_buffer((i2s_port_t)MIC_I2S_PORT_NUM);
    delay(150);

    s_initialized = true;
    return true;
#endif
}

bool MicCapture_Start() {
    if (!s_initialized) return false;
#if !ENABLE_MIC_CAPTURE
    s_running = true;
    s_level   = 0.0f;
    return true;
#else
    i2s_zero_dma_buffer((i2s_port_t)MIC_I2S_PORT_NUM);
    if (i2s_start((i2s_port_t)MIC_I2S_PORT_NUM) != ESP_OK) {
        return false;
    }
    s_running = true;
    s_level   = 0.0f;
    return true;
#endif
}

bool MicCapture_Stop() {
    if (!s_running) return true;
#if !ENABLE_MIC_CAPTURE
    s_running = false;
    s_level   = 0.0f;
    return true;
#else
    i2s_stop((i2s_port_t)MIC_I2S_PORT_NUM);
    s_running = false;
    s_level   = 0.0f;
    return true;
#endif
}

void MicCapture_Deinit() {
#if !ENABLE_MIC_CAPTURE
    s_initialized = false;
    s_running     = false;
#else
    if (!s_initialized) return;
    i2s_stop((i2s_port_t)MIC_I2S_PORT_NUM);
    i2s_driver_uninstall((i2s_port_t)MIC_I2S_PORT_NUM);
    s_initialized = false;
    s_running     = false;
#endif
}

bool MicCapture_IsRunning() {
    return s_running;
}

size_t MicCapture_ReadChunk(int16_t* out_buffer, size_t max_samples) {
    if (!s_running || !out_buffer || max_samples == 0) return 0;

#if !ENABLE_MIC_CAPTURE
    // stub：返回 0，配合上层 WS 推送在物理设备到位前不会发出垃圾帧
    return 0;
#else
    const size_t want = (max_samples < (size_t)MIC_CHUNK_SAMPLES)
                            ? max_samples
                            : (size_t)MIC_CHUNK_SAMPLES;
    // INMP441 用 32-bit 帧采集；本地栈缓冲读 32-bit，再右移 14 位下变频到 int16
    static int32_t s_raw[ MIC_CHUNK_SAMPLES ];
    size_t bytes_read = 0;
    esp_err_t err = i2s_read((i2s_port_t)MIC_I2S_PORT_NUM, s_raw,
                             want * sizeof(int32_t), &bytes_read,
                             portMAX_DELAY);
    if (err != ESP_OK) return 0;
    const size_t samples = bytes_read / sizeof(int32_t);
    for (size_t i = 0; i < samples; i++) {
        // INMP441: 32-bit 帧高 24 位是数据。右移 14 位后 cast int16 拿到
        // 接近 16-bit 量化的 PCM，与 ASR 16kHz/16bit 输入对齐。
        out_buffer[i] = (int16_t)(s_raw[i] >> 14);
    }
    if (samples > 0) {
        const float rms = ComputeRms16(out_buffer, samples);
        // 一阶低通平滑，避免 LEVEL 条左右乱跳
        s_level = 0.7f * s_level + 0.3f * rms;
        if (s_level > 1.0f) s_level = 1.0f;
    }
    return samples;
#endif
}

float MicCapture_GetLevel() {
    return s_running ? s_level : 0.0f;
}
