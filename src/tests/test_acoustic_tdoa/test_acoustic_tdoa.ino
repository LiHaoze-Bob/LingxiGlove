/**
 * test_acoustic_tdoa.ino — D2: 双手手语翻译声学 TDOA 测距 POC
 * ---------------------------------------------------------------
 * 单个 sketch 通过修改一行 #define（ROLE_TX / ROLE_RX）即可烧录成
 * "发端"或"收端"两种角色，用于双 ESP32-S3 板之间做 17–19 kHz chirp
 * 的飞行时间测距实验。
 *
 *   TX 端 (ROLE_TX):
 *     - 用 MAX98357A (I2S0, TX 模式) 每 500ms 播放一次 chirp_pcm.h 里的
 *       17-19kHz 线性 chirp 模板 (48 kHz / 5 ms / 240 samples)
 *     - 同时通过 D3 (kTrigPin) 拉高一个短脉冲作为"发射时刻"参考信号
 *       （可选：供示波器查看端到端延迟；RX 端不依赖此线）
 *
 *   RX 端 (ROLE_RX):
 *     - 用 INMP441 (I2S1, RX 模式, 48 kHz int16) 持续采集
 *     - 在 ~30 ms 的滑动窗口上，用定点 int16×int16→int64 朴素滑动相关
 *       算出与 chirp 模板对齐最好的样本偏移 k*
 *     - 为什么不用 FFT：240 × 1200 = 288k 乘加，ESP32-S3 单核 240MHz
 *       单次 <3ms，且不吃 SRAM / 浮点栈，后期好搬进主程序
 *
 * ⚠️ 重要：当前 POC 打印的是什么数字？（诚实披露，避免误读）
 * ----------------------------------------------------------
 * 本 POC **没有**跨板硬同步，因此 RX 算到的 argmax 只是"chirp 在
 * 30 ms 采集窗口里的起始位置"，**不是 TX→RX 的绝对飞行时间**，
 * 所以也**不是真实两手距离**。我们打印两个量：
 *   1) window_offset_samples：chirp 在窗口里的位置（原始可验证数据）
 *   2) rel_delta_mm：相邻两次测量的 window_offset 差值 × c / fs
 *      —— 这才是无硬同步下**唯一物理上有意义**的量：
 *         双手相对位移随时间的变化量
 * 把 rel_delta_mm 累加即可得到"相对于首次测量的相对距离变化"；
 * 真实绝对距离要等 D3（真机接入 esp_now_sync.h 的硬触发）才能测。
 *
 * 硬件约束（使用 Arduino Nano ESP32 ABX00083 的 Dx 命名常量，
 * 由 Arduino core 负责映射到底层 GPIO；同样适配 ESP32-S3 DevKit）：
 *   MAX98357A (TX 角色板接，I2S_NUM_0):
 *     BCLK -> D4   (和主程序 config.h 的 I2S_BCLK 完全一致)
 *     LRC  -> D5
 *     DIN  -> D6
 *     SD   -> GND (右声道 mono)
 *     VCC  -> 3V3 或 5V
 *     GND  -> GND
 *
 *   INMP441 (RX 角色板接，I2S_NUM_1):
 *     SCK  -> D10   (Nano ESP32 的 SPI CS 脚，默认 SPI 未启用时可复用)
 *     WS   -> D11   (Nano ESP32 的 SPI COPI 脚)
 *     SD   -> D12   (Nano ESP32 的 SPI CIPO 脚)
 *     L/R  -> GND
 *     VCC  -> 3V3
 *     GND  -> GND
 *   若本 sketch 和 SPI 外设冲突，把下方 kRxBclkPin / kRxWsPin / kRxDataPin
 *   改成其他任意空闲 Dx 即可（ESP32-S3 IO-MUX 允许任意引脚做 I2S）。
 *
 *   可选示波器触发（kTrigPin）: D3 — 仅 TX 角色使用，观察端到端延迟
 *
 * 编译方式：
 *   Arduino IDE → 板子选 "Arduino Nano ESP32 (ABX00083)"
 *   下方 #define ROLE_TX / ROLE_RX 只保留一个（另一个注释掉），分别
 *   烧录到两块板。保留错误组合会触发 #error 立即拒编译。
 *
 * 与 D1 仿真的对照（D3 真机实测阶段用）：
 *   - chirp 参数完全一致 (48k/5ms/17-19kHz)
 *   - 相关器算法一致（整数样本 argmax，无亚样本插值）
 *   - 预期：SNR ≥ 10 dB 时 window_offset 的 std <= 2 个样本 (c/fs≈7mm
 *     量化地板下，std = 1 样本对应 7 mm rel_delta 随机波动)
 *
 * ⚠️ 本 sketch 只做 POC 可行性验证，不嵌入 LingxiGlove_Main 主程序。
 */

// ==========  角色编译开关（只保留一个 #define） ==========
#define ROLE_TX
// #define ROLE_RX

#if defined(ROLE_TX) && defined(ROLE_RX)
#  error "ROLE_TX and ROLE_RX cannot both be defined. Pick exactly one."
#endif
#if !defined(ROLE_TX) && !defined(ROLE_RX)
#  error "Must define exactly one of ROLE_TX / ROLE_RX."
#endif

#include <Arduino.h>
#include <driver/i2s.h>
#include "chirp_pcm.h"

// ========== 通用常量 ==========
static const uint32_t kSerialBaud = 115200u;
static const float    kSpeedOfSound_m_s = 343.0f;  // 20°C 干空气，可按温度修正

// 发射端示波器触发脉冲（RX 不依赖）。
// 使用 Arduino Nano ESP32 的 Dx 名义常量而非裸 GPIO 号：Arduino core 会在
// 编译期把 D3 正确映射到 ABX00083 的实际 GPIO；同名 sketch 搬去 ESP32-S3
// DevKit 时也无需改动（那里 D3 == GPIO3）。
static const int kTrigPin = D3;

// ==============================================================
//                         TX  (发射端)
// ==============================================================
#ifdef ROLE_TX

static const i2s_port_t kTxI2sPort = I2S_NUM_0;
// 与主程序 config.h 里的 I2S_BCLK/LRC/DIN 完全一致，沿用 Dx 命名。
static const int kTxBclkPin = D4;
static const int kTxLrcPin  = D5;
static const int kTxDinPin  = D6;
static const uint32_t kTxIntervalMs = 500u;

static bool InitTxI2s() {
    i2s_config_t cfg = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
        .sample_rate = kChirpSampleRate,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 4,
        .dma_buf_len = 256,
        .use_apll = false,
        .tx_desc_auto_clear = true,
        .fixed_mclk = 0
    };
    i2s_pin_config_t pins = {
        .bck_io_num = kTxBclkPin,
        .ws_io_num = kTxLrcPin,
        .data_out_num = kTxDinPin,
        .data_in_num = I2S_PIN_NO_CHANGE
    };
    if (i2s_driver_install(kTxI2sPort, &cfg, 0, NULL) != ESP_OK) {
        Serial.println(F("[TX] i2s_driver_install failed"));
        return false;
    }
    if (i2s_set_pin(kTxI2sPort, &pins) != ESP_OK) {
        Serial.println(F("[TX] i2s_set_pin failed"));
        i2s_driver_uninstall(kTxI2sPort);
        return false;
    }
    return true;
}

static void PlayChirpOnce() {
    // 拷贝到栈上的 RAM 缓冲再 i2s_write（部分 ESP32 版本 i2s_write 不接受 PROGMEM 指针）。
    // 用 kChirpLen 作为数组长度，避免硬编码 240；kChirpLen 是 constexpr 可用作 ICE。
    static int16_t s_chirp_ram[kChirpLen];
    static bool s_copied = false;
    if (!s_copied) {
        for (uint32_t i = 0; i < kChirpLen; ++i) {
            s_chirp_ram[i] = (int16_t)pgm_read_word(&kChirpPcm[i]);
        }
        s_copied = true;
    }

    digitalWrite(kTrigPin, HIGH);
    size_t bytes_written = 0;
    (void)i2s_write(kTxI2sPort, s_chirp_ram,
                    kChirpLen * sizeof(int16_t),
                    &bytes_written, portMAX_DELAY);
    digitalWrite(kTrigPin, LOW);
}

void setup() {
    Serial.begin(kSerialBaud);
    while (!Serial && millis() < 2000) { /* wait up to 2s */ }
    Serial.println();
    Serial.println(F("================================"));
    Serial.println(F(" LingxiGlove D2: Acoustic TDOA TX"));
    Serial.println(F("================================"));
    Serial.print(F("[TX] chirp: "));
    Serial.print(kChirpF0Hz); Serial.print(F(" -> "));
    Serial.print(kChirpF1Hz); Serial.print(F(" Hz, "));
    Serial.print(kChirpDurMs); Serial.println(F(" ms, 48 kHz"));

    pinMode(kTrigPin, OUTPUT);
    digitalWrite(kTrigPin, LOW);

    if (!InitTxI2s()) {
        Serial.println(F("[TX] I2S init FAILED, halting"));
        while (1) { delay(1000); }
    }
    Serial.println(F("[TX] I2S ready, start emitting chirps"));
}

void loop() {
    PlayChirpOnce();
    Serial.print(F("[TX] chirp emitted @ t = "));
    Serial.print(millis());
    Serial.println(F(" ms"));
    delay(kTxIntervalMs);
}

#endif  // ROLE_TX


// ==============================================================
//                         RX  (接收端)
// ==============================================================
#ifdef ROLE_RX

static const i2s_port_t kRxI2sPort = I2S_NUM_1;
// 复用 Nano ESP32 默认 SPI (D10 CS / D11 COPI / D12 CIPO)，本 sketch 不启用 SPI
// 时这三个口空闲；若用户的项目同时用 SPI，改成任意其它空闲 Dx 即可。
static const int kRxBclkPin = D10;
static const int kRxWsPin   = D11;
static const int kRxDataPin = D12;

// 采集窗口：~30 ms 足以覆盖 1.5 m 内的回波最大延迟 + 模板自身 5 ms
static const uint32_t kRxWindowSamples = 1440u;  // 30 ms @ 48 kHz

// ------------------ 检测判据（自适应，不凭经验值） ------------------
// 问题：早期版本用 kCorrPeakThreshold = 20_000_000 绝对阈值，但那只是
// "我猜大概这个数量级"，没有真实数据支撑。对不同 INMP441 模组 / 不同音量 /
// 不同距离，相关峰绝对值能差 1~2 个数量级，绝对阈值必然错。
// 解决：改成 "主峰相对次峰的显著性比值"，属于 CFAR（恒虚警）思想：
//   1) 找到 argmax 的绝对值 peak_abs
//   2) 在远离 argmax ±kGuardSamples 的"保护带"外，找第二大绝对值 secondary
//   3) 仅当 peak_abs >= kSignificanceRatio * secondary 才判定为"有效 chirp"
// 这个判据对信号能量绝对值不敏感，只看"主瓣是不是显著突起"，物理意义
// 也更清晰（"chirp 匹配滤波峰值必然比非 chirp 段高几倍"）。
static const uint32_t kGuardSamples      = 32u;   // 主峰两侧 32 样本 = 666us
                                                  // = chirp 主瓣宽度的 5-10×
static const int64_t  kSignificanceRatio = 4LL;   // 主峰至少 4× 于次峰

// 采集缓冲（固定分配在 .bss，避免每次 loop 分配）
static int16_t s_rx_buf[kRxWindowSamples];
// 用 kChirpLen 作为数组长度；不再硬编码 240（kChirpLen 是 constexpr）
static int16_t s_chirp_ram[kChirpLen];
static bool    s_chirp_copied = false;

// 相对位移相关状态：跨 loop 保留上一次有效的 window_offset 样本号
static bool     s_has_prev_offset     = false;
static uint32_t s_prev_offset_samples = 0;

static bool InitRxI2s() {
    i2s_config_t cfg = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
        .sample_rate = kChirpSampleRate,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,
        .dma_buf_len  = 256,
        .use_apll = false,
        .tx_desc_auto_clear = false,
        .fixed_mclk = 0
    };
    i2s_pin_config_t pins = {
        .bck_io_num = kRxBclkPin,
        .ws_io_num  = kRxWsPin,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num  = kRxDataPin
    };
    if (i2s_driver_install(kRxI2sPort, &cfg, 0, NULL) != ESP_OK) {
        Serial.println(F("[RX] i2s_driver_install failed"));
        return false;
    }
    if (i2s_set_pin(kRxI2sPort, &pins) != ESP_OK) {
        Serial.println(F("[RX] i2s_set_pin failed"));
        i2s_driver_uninstall(kRxI2sPort);
        return false;
    }
    // INMP441 上电需 ~100ms 冷启动；读走 DMA 内的噪声数据
    i2s_zero_dma_buffer(kRxI2sPort);
    delay(150);
    return true;
}

// 朴素定点滑动相关：返回 (argmax, peak_abs, secondary_abs)
// - peak_abs      = |corr[argmax]|
// - secondary_abs = 去掉 [argmax - guard, argmax + guard] 保护带后的次大 |corr|
// 复杂度 O(M*(N-M+1)) ≈ 240 * 1200 = 288k MAC，单核 240MHz 单次 <3 ms。
// 用两趟 pass：第 1 趟找 argmax，第 2 趟在保护带外找次大值。这比一趟
// "同时维护 top-2 + 带保护"简单，代码量少不易错。
static void SlidingCorrInt16(const int16_t* rx, uint32_t rx_len,
                             const int16_t* tmpl, uint32_t tmpl_len,
                             uint32_t guard_samples,
                             uint32_t* out_argmax,
                             int64_t*  out_peak_abs,
                             int64_t*  out_secondary_abs) {
    *out_argmax = 0;
    *out_peak_abs = 0;
    *out_secondary_abs = 0;
    if (rx_len < tmpl_len) {
        return;
    }
    const uint32_t k_end = rx_len - tmpl_len;  // inclusive upper bound

    // pass 1: argmax + |peak|
    int64_t  best_abs = 0;
    uint32_t best_k   = 0;
    for (uint32_t k = 0; k <= k_end; ++k) {
        int64_t acc = 0;
        for (uint32_t j = 0; j < tmpl_len; ++j) {
            acc += (int32_t)rx[k + j] * (int32_t)tmpl[j];
        }
        int64_t abs_acc = acc < 0 ? -acc : acc;
        if (abs_acc > best_abs) {
            best_abs = abs_acc;
            best_k = k;
        }
    }
    *out_argmax = best_k;
    *out_peak_abs = best_abs;

    // pass 2: secondary |peak| outside [best_k - guard, best_k + guard]
    // 保护带用饱和减法避免 uint32 下溢
    uint32_t guard_lo = (best_k > guard_samples) ? (best_k - guard_samples) : 0u;
    uint32_t guard_hi = best_k + guard_samples;  // 可以超过 k_end，比较时自动裁剪
    int64_t second_abs = 0;
    for (uint32_t k = 0; k <= k_end; ++k) {
        if (k >= guard_lo && k <= guard_hi) {
            continue;  // 主峰保护带内跳过
        }
        int64_t acc = 0;
        for (uint32_t j = 0; j < tmpl_len; ++j) {
            acc += (int32_t)rx[k + j] * (int32_t)tmpl[j];
        }
        int64_t abs_acc = acc < 0 ? -acc : acc;
        if (abs_acc > second_abs) {
            second_abs = abs_acc;
        }
    }
    *out_secondary_abs = second_abs;
}

static bool ReadFullWindow() {
    // I2S 一次 read 不保证填满；循环直到拿到 kRxWindowSamples 个 int16
    uint8_t* dst = reinterpret_cast<uint8_t*>(s_rx_buf);
    const size_t need_bytes = kRxWindowSamples * sizeof(int16_t);
    size_t got = 0;
    while (got < need_bytes) {
        size_t n = 0;
        esp_err_t err = i2s_read(kRxI2sPort, dst + got,
                                 need_bytes - got, &n,
                                 pdMS_TO_TICKS(100));
        if (err != ESP_OK) {
            Serial.print(F("[RX] i2s_read err="));
            Serial.println((int)err);
            return false;
        }
        got += n;
    }
    return true;
}

void setup() {
    Serial.begin(kSerialBaud);
    while (!Serial && millis() < 2000) { /* wait up to 2s */ }
    Serial.println();
    Serial.println(F("================================"));
    Serial.println(F(" LingxiGlove D2: Acoustic TDOA RX"));
    Serial.println(F("================================"));
    Serial.print(F("[RX] chirp template: "));
    Serial.print(kChirpLen);
    Serial.println(F(" samples @ 48 kHz"));
    Serial.print(F("[RX] rx window: "));
    Serial.print(kRxWindowSamples);
    Serial.println(F(" samples (~30 ms)"));
    Serial.print(F("[RX] detect criterion: peak >= "));
    Serial.print((long)kSignificanceRatio);
    Serial.print(F("x secondary, guard band = "));
    Serial.print(kGuardSamples);
    Serial.println(F(" samples"));

    if (!s_chirp_copied) {
        for (uint32_t i = 0; i < kChirpLen; ++i) {
            s_chirp_ram[i] = (int16_t)pgm_read_word(&kChirpPcm[i]);
        }
        s_chirp_copied = true;
    }

    if (!InitRxI2s()) {
        Serial.println(F("[RX] I2S init FAILED, halting"));
        while (1) { delay(1000); }
    }
    Serial.println(F("[RX] ready, listening..."));
}

void loop() {
    if (!ReadFullWindow()) {
        delay(10);
        return;
    }

    uint32_t argmax        = 0;
    int64_t  peak_abs      = 0;
    int64_t  secondary_abs = 0;
    SlidingCorrInt16(s_rx_buf, kRxWindowSamples,
                     s_chirp_ram, kChirpLen,
                     kGuardSamples,
                     &argmax, &peak_abs, &secondary_abs);

    // CFAR 判据：主峰必须显著高于保护带外的次峰
    // 用 "peak >= ratio * second" 避免除法（second 可能 == 0）
    const bool is_valid_chirp =
        (secondary_abs > 0)
            ? (peak_abs >= (int64_t)kSignificanceRatio * secondary_abs)
            : (peak_abs > 0);  // 全窗无次峰（不可能在真实噪声下发生，兜底）

    if (!is_valid_chirp) {
        return;  // 本窗未检测到显著主瓣
    }

    // 诚实输出：window_offset 是 chirp 在"30ms 采集窗口"里的位置，
    // 在没有跨板硬同步的情况下，它不等于 TX→RX 飞行时间。
    // 真正有物理意义的是：相邻两次有效测量之间 window_offset 的变化量
    //    Δk = k_now − k_prev
    //    Δd = Δk / fs × c  →  **双手相对位移量**
    // 这个相对量不依赖绝对同步，只依赖双板 I2S 的采样率稳定性。
    int32_t delta_samples = 0;
    float   rel_delta_mm  = 0.0f;
    const bool has_delta  = s_has_prev_offset;
    if (has_delta) {
        delta_samples = (int32_t)argmax - (int32_t)s_prev_offset_samples;
        rel_delta_mm  = (float)delta_samples
                        / (float)kChirpSampleRate
                        * kSpeedOfSound_m_s * 1000.0f;
    }
    s_prev_offset_samples = argmax;
    s_has_prev_offset     = true;

    Serial.print(F("[RX] chirp: win_offset="));
    Serial.print(argmax);
    Serial.print(F(" samples, peak/2nd="));
    // 打印信噪显著性比值（保留 2 位小数）
    if (secondary_abs > 0) {
        const float sig_ratio = (float)peak_abs / (float)secondary_abs;
        Serial.print(sig_ratio, 2);
    } else {
        Serial.print(F("inf"));
    }
    if (has_delta) {
        Serial.print(F(", rel_delta="));
        Serial.print(rel_delta_mm, 1);
        Serial.print(F(" mm"));
    }
    Serial.println();
}

#endif  // ROLE_RX
