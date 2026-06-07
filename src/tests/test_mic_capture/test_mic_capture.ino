// ============================================================
// test_mic_capture.ino
// 验证 INMP441 麦克风 I2S 接线是否正常（16 kHz / 16-bit / mono）
//
// 接线：
//   SCK  -> D10 (I2S1 BCLK)
//   WS   -> D11 (I2S1 LRCLK)
//   SD   -> D12 (I2S1 DIN)
//   L/R  -> GND (左声道)
//   VCC  -> 3V3
//   GND  -> GND
//
// 用法：
//   1. 在 Arduino IDE 选 "Arduino Nano ESP32" 板子
//   2. 上传后打开 Serial Monitor（115200 baud）
//   3. 对着麦克风说话/拍手，观察 RMS 变化
//      - 安静环境：RMS < 500
//      - 说话/拍手：RMS > 2000（越近越大）
//      - 恒 0 = 接线有误或 L/R 没接 GND
//
// 输出格式（每 200ms 一行）：
//   [MIC] RMS=1234  peak=5678  samples=512  ████████░░░░░░░░ (level bar)
// ============================================================

#include <Arduino.h>
#include "driver/i2s.h"

// ---------- I2S 引脚配置（与 config.h 保持一致） ----------
static const i2s_port_t I2S_PORT   = I2S_NUM_1;
static const int PIN_BCLK          = D10;
static const int PIN_LRCLK         = D11;
static const int PIN_DIN           = D12;

// ---------- 采样参数（ASR 场景：16kHz / 16-bit / mono） ----------
static const uint32_t SAMPLE_RATE  = 16000;
static const uint32_t DMA_BUF_COUNT = 8;
static const uint32_t DMA_BUF_LEN   = 256;
static const uint32_t READ_SAMPLES  = 512;   // 每次读 512 样本 = 32 ms

// PCM 缓冲：
//   - I2S 直接读到 32-bit raw（INMP441 实际输出 24-bit 左对齐，需 32-bit 帧）
//   - 转换后存到 16-bit out 缓冲，给 RMS / 后续 ASR 用
static int32_t s_raw_buffer[READ_SAMPLES];
static int16_t s_buffer[READ_SAMPLES];

// ---------- I2S 初始化 ----------
static bool initI2sMic() {
    i2s_config_t cfg = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
        .sample_rate = SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,  // INMP441 必须 32bit
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,    // L/R=GND → 数据在 LEFT 时隙
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = (int)DMA_BUF_COUNT,
        .dma_buf_len  = (int)DMA_BUF_LEN,
        .use_apll = false,
        .tx_desc_auto_clear = false,
        .fixed_mclk = 0
    };
    i2s_pin_config_t pins = {
        // 必须用 digitalPinToGPIONumber() 把 Arduino Dx 抽象 pin 号转换成真实
        // ESP32 GPIO 号——i2s_set_pin 是 ESP-IDF API，不会触发 Arduino 框架的
        // pin remap。直接传 D10/D11/D12 会被当成 GPIO10/11/12 物理脚使用，
        // 导致 I2S 信号驱动到错误位置（项目通用坑，所有 ESP-IDF 直接调用通用）。
        .bck_io_num   = digitalPinToGPIONumber(PIN_BCLK),
        .ws_io_num    = digitalPinToGPIONumber(PIN_LRCLK),
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num  = digitalPinToGPIONumber(PIN_DIN)
    };

    esp_err_t err = i2s_driver_install(I2S_PORT, &cfg, 0, NULL);
    if (err != ESP_OK) {
        Serial.printf("[MIC] i2s_driver_install FAILED: %d\n", err);
        return false;
    }
    err = i2s_set_pin(I2S_PORT, &pins);
    if (err != ESP_OK) {
        Serial.printf("[MIC] i2s_set_pin FAILED: %d\n", err);
        i2s_driver_uninstall(I2S_PORT);
        return false;
    }

    // INMP441 冷启动约 100ms，清空 DMA 噪声
    i2s_zero_dma_buffer(I2S_PORT);
    delay(150);
    Serial.println("[MIC] I2S1 初始化成功 (16kHz/16-bit/mono)");
    return true;
}

// ---------- 电平条可视化 ----------
static void printLevelBar(uint16_t rms) {
    // 按 RMS 值映射到 0-16 格
    const int maxBar = 16;
    int level = map(constrain(rms, 0, 8000), 0, 8000, 0, maxBar);
    for (int i = 0; i < maxBar; i++) {
        Serial.print(i < level ? "\xe2\x96\x88" : "\xe2\x96\x91");  // █ or ░
    }
}

// ---------- setup ----------
void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("\n========================================");
    Serial.println("  INMP441 麦克风接线验证 (test_mic_capture)");
    Serial.println("  采样率: 16 kHz | 位深: 16-bit | 通道: mono");
    Serial.println("  引脚: BCLK=D10, LRCLK=D11, DIN=D12");
    Serial.println("========================================\n");

    // 打印实际 GPIO 映射（Arduino Nano ESP32 的 Dx 在 By Arduino pin 模式下
    // 是抽象 pin 号，需要 digitalPinToGPIONumber() 转换成真实 GPIO 才能给
    // ESP-IDF 直接 API 用）。
    Serial.printf("[PIN] D10 -> Arduino pin=%d, real GPIO=%d (BCLK / SCK)\n",
                  PIN_BCLK, digitalPinToGPIONumber(PIN_BCLK));
    Serial.printf("[PIN] D11 -> Arduino pin=%d, real GPIO=%d (LRCLK / WS)\n",
                  PIN_LRCLK, digitalPinToGPIONumber(PIN_LRCLK));
    Serial.printf("[PIN] D12 -> Arduino pin=%d, real GPIO=%d (DIN / SD)\n",
                  PIN_DIN, digitalPinToGPIONumber(PIN_DIN));
    Serial.println("[PIN] 期望真实 GPIO: D10=GPIO21, D11=GPIO38, D12=GPIO47\n");

    // ---------- GPIO 物理探针：诊断每根线的硬件连接状态 ----------
    // 原理：把 I2S 还没初始化的引脚分别设成 INPUT_PULLUP 和 INPUT_PULLDOWN，
    // 读取电平，根据组合判断该 GPIO 是浮空、还是被外部低阻拉到某电源/地。
    //   - 上拉读1 + 下拉读0 → 悬空（外部没接东西）
    //   - 上拉读1 + 下拉读1 → 外部强上拉（接到 VCC 或被外设拉高）
    //   - 上拉读0 + 下拉读0 → 外部强下拉（接到 GND 或被外设拉低）
    auto probePin = [](const char* name, int arduinoPin) {
        pinMode(arduinoPin, INPUT_PULLUP);
        delayMicroseconds(50);
        int up = digitalRead(arduinoPin);
        pinMode(arduinoPin, INPUT_PULLDOWN);
        delayMicroseconds(50);
        int dn = digitalRead(arduinoPin);
        const char* verdict;
        if (up == 1 && dn == 0)      verdict = "FLOATING (悬空，未接外部信号)";
        else if (up == 1 && dn == 1) verdict = "STRONG-HIGH (被外部拉高，可能接 VCC)";
        else if (up == 0 && dn == 0) verdict = "STRONG-LOW  (被外部拉低，可能接 GND 或模块未上电)";
        else                          verdict = "UNSTABLE (诡异状态)";
        Serial.printf("[PROBE] %s pullup=%d pulldown=%d  -> %s\n",
                      name, up, dn, verdict);
        // 还原成 INPUT 让 I2S 后续接管
        pinMode(arduinoPin, INPUT);
    };
    Serial.println("---- GPIO 物理状态探针 (I2S 初始化前) ----");
    probePin("D10/BCLK", PIN_BCLK);
    probePin("D11/WS  ", PIN_LRCLK);
    probePin("D12/SD  ", PIN_DIN);
    Serial.println();

    // ---------- GPIO 自检：验证 ESP32 内部 GPIO47 没坏 ----------
    // 主动把 D12 拉高/拉低再读，绕开外部接线，确认 ESP32 自身 GPIO 正常。
    auto selfTestPin = [](const char* name, int arduinoPin) {
        pinMode(arduinoPin, OUTPUT);
        digitalWrite(arduinoPin, HIGH);
        delayMicroseconds(50);
        int rh = digitalRead(arduinoPin);
        digitalWrite(arduinoPin, LOW);
        delayMicroseconds(50);
        int rl = digitalRead(arduinoPin);
        Serial.printf("[SELFTEST] %s drive_high_read=%d drive_low_read=%d  -> %s\n",
                      name, rh, rl,
                      (rh == 1 && rl == 0) ? "OK (内部GPIO正常)"
                                           : "BAD (GPIO物理损坏!)");
        pinMode(arduinoPin, INPUT);  // 还原
    };
    Serial.println("---- ESP32 内部 GPIO 自检 ----");
    selfTestPin("D10/BCLK", PIN_BCLK);
    selfTestPin("D11/WS  ", PIN_LRCLK);
    selfTestPin("D12/SD  ", PIN_DIN);
    Serial.println();

    // ---------- INMP441 上电状态判定 ----------
    // 思路：INMP441 即使在 idle（没 BCLK）时，SD 上电后通常处于 Hi-Z，
    // 而模块的 VCC/GND 焊盘到 INMP441 SD 焊盘之间存在内部弱关联——
    // 这里我们让 D12 短暂输出高电平后立即切到 INPUT（不开上下拉），
    // 由于 ESP32 内部和外部都没主动驱动，GPIO47 上的电荷只能通过
    // 外部连接的 INMP441 SD 引脚释放。如果外部线断开，电荷无处释放，
    // 引脚会保持高电平（受 GPIO 漏电缓慢衰减）；如果外部连接正常且
    // INMP441 工作中，SD 会在 us 级被拉到三态/低电平。
    Serial.println("---- INMP441 上电耦合检查 (D12) ----");
    pinMode(PIN_DIN, OUTPUT);
    digitalWrite(PIN_DIN, HIGH);
    delayMicroseconds(100);
    pinMode(PIN_DIN, INPUT);  // 撤掉驱动，看电平多快被拉走
    delayMicroseconds(5);
    int t5 = digitalRead(PIN_DIN);
    delayMicroseconds(50);
    int t55 = digitalRead(PIN_DIN);
    delay(5);
    int t5ms = digitalRead(PIN_DIN);
    Serial.printf("[COUPLE] D12 撤驱后 t+5us=%d t+55us=%d t+5ms=%d ", t5, t55, t5ms);
    if (t5 == 1 && t55 == 1 && t5ms == 1) {
        Serial.println(" -> 极可能 D12 完全悬空 (线断 / 排针接触不良)");
    } else {
        Serial.println(" -> D12 与外部电路有耦合 (线连接 OK)");
    }
    Serial.println();

    if (!initI2sMic()) {
        Serial.println("[MIC] *** 初始化失败! 请检查接线 ***");
        while (1) { delay(1000); }
    }

    Serial.println("[MIC] 开始采集，请对着麦克风说话或拍手...\n");
    Serial.println("  判读: RMS恒=0 → 接线有误");
    Serial.println("  判读: RMS<500且稳定 → 安静环境正常");
    Serial.println("  判读: 说话时RMS>2000 → 麦克风工作正常\n");
}

// ---------- loop ----------
static uint32_t s_lastPrint = 0;
static uint32_t s_lastRawDump = 0;
static uint32_t s_totalReads = 0;
static uint32_t s_zeroReads = 0;

void loop() {
    size_t bytes_read = 0;
    esp_err_t err = i2s_read(I2S_PORT, s_raw_buffer, sizeof(s_raw_buffer), &bytes_read, portMAX_DELAY);
    if (err != ESP_OK || bytes_read == 0) {
        return;
    }

    const size_t samples = bytes_read / sizeof(int32_t);
    s_totalReads++;

    // 同时统计 raw 32-bit 层和 16-bit 下变频后的 peak/RMS
    int64_t sum_sq = 0;
    int16_t peak_val = 0;
    int32_t raw_peak_abs = 0;
    bool all_zero = true;
    for (size_t i = 0; i < samples; i++) {
        int32_t raw = s_raw_buffer[i];
        int32_t raw_abs = (raw < 0) ? -raw : raw;
        if (raw_abs > raw_peak_abs) raw_peak_abs = raw_abs;

        int16_t v = (int16_t)(raw >> 14);
        s_buffer[i] = v;
        sum_sq += (int64_t)v * v;
        int16_t abs_v = (v < 0) ? -v : v;
        if (abs_v > peak_val) peak_val = abs_v;
        if (raw != 0) all_zero = false;
    }
    if (all_zero) s_zeroReads++;

    uint16_t rms = (uint16_t)sqrt((double)sum_sq / samples);

    // 每 200ms 输出一次 RMS/电平
    uint32_t now = millis();
    if (now - s_lastPrint >= 200) {
        s_lastPrint = now;
        Serial.printf("[MIC] RMS=%-5u peak16=%-5d rawPeak=0x%08lx  ",
                      rms, peak_val, (long)raw_peak_abs);
        printLevelBar(rms);
        if (s_zeroReads > 10 && s_totalReads > 0 &&
            (s_zeroReads * 100 / s_totalReads) > 90) {
            Serial.print("  ⚠️ raw 全零!");
        }
        Serial.println();
    }

    // 每 2 秒打印 8 个原始 32-bit 样本（hex），帮助判断接线/格式
    if (now - s_lastRawDump >= 2000) {
        s_lastRawDump = now;
        Serial.print("[RAW] ");
        for (size_t i = 0; i < 8 && i < samples; i++) {
            Serial.printf("%08lx ", (unsigned long)(uint32_t)s_raw_buffer[i]);
        }
        Serial.println();
    }
}
