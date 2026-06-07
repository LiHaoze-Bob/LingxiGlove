// ============================================================
// test_flex_read.ino
// 弯曲传感器硬件验证 sketch（独立运行，不依赖主项目）
// ============================================================
// 用途：验证 flex 传感器接线是否正确、ADC 读数是否随弯曲变化。
// 烧录到 Slave 板（Arduino Nano ESP32-S3），打开串口监视器 115200。
//
// 预期输出（每 500ms 一次）：
//   [Flex] A2(GPIO3): raw=2345  volts=1.89V
// 弯曲手指时 raw 值应明显变化（通常下降）。
// 若 raw 始终为 0 或 4095 → 接线有问题（断路或短路）。
// ============================================================

// ---- 引脚配置（与 config.h 保持一致） ----
// Arduino Nano ESP32-S3 引脚映射：
//   A0=GPIO1, A1=GPIO2, A2=GPIO3, A3=GPIO4,
//   A6=GPIO13, A7=GPIO14
//   A4/A5 被 I2C (MPU6050) 占用，不可用

#define FLEX_PIN_CANDIDATE_3   3     // GPIO3 模式下的 A2
#define FLEX_PIN_CANDIDATE_19  19    // D-number 模式下的 A2

#define SAMPLE_INTERVAL_MS  500    // 采样间隔
#define OVERSAMPLE_COUNT    8      // 过采样次数（降噪）

void setup() {
    Serial.begin(115200);
    delay(1000);

    Serial.println("\n========================================");
    Serial.println("  LingxiGlove Flex A2 引脚定位");
    Serial.println("========================================");
    Serial.printf("同时读 pin3(GPIO3模式) 和 pin19(D-number模式)\n");
    Serial.printf("A2常量值=%d, A0常量值=%d\n", A2, A0);
    Serial.println("弯曲手指看哪个引脚 raw 值变化\n");

    analogReadResolution(12);
    pinMode(FLEX_PIN_CANDIDATE_3, INPUT);
    pinMode(FLEX_PIN_CANDIDATE_19, INPUT);
}

void loop() {
    // 读 pin 3
    uint32_t acc3 = 0;
    for (int i = 0; i < OVERSAMPLE_COUNT; i++) acc3 += analogRead(FLEX_PIN_CANDIDATE_3);
    uint16_t raw3 = (uint16_t)(acc3 / OVERSAMPLE_COUNT);

    // 读 pin 19
    uint32_t acc19 = 0;
    for (int i = 0; i < OVERSAMPLE_COUNT; i++) acc19 += analogRead(FLEX_PIN_CANDIDATE_19);
    uint16_t raw19 = (uint16_t)(acc19 / OVERSAMPLE_COUNT);

    Serial.printf("pin%2d: raw=%4u (%.2fV)  |  pin%2d: raw=%4u (%.2fV)\n",
                  FLEX_PIN_CANDIDATE_3, raw3, (double)(raw3*3.3f/4095.0f),
                  FLEX_PIN_CANDIDATE_19, raw19, (double)(raw19*3.3f/4095.0f));

    Serial.println("---");
    delay(SAMPLE_INTERVAL_MS);
}
