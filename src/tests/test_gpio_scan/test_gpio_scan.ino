// ============================================================
// test_gpio_scan.ino
// GPIO 扫描诊断工具 — 找出扩展板 A2 对应的真实 GPIO
// ============================================================
// 使用方法：
//   1. 用杜邦线将扩展板 A2 排针接到扩展板 GND
//   2. 烧录本 sketch 到 Slave 板
//   3. 打开串口监视器 115200
//   4. 观察哪个 GPIO 读数接近 0 → 那个就是 A2 的真实 GPIO
//
// 扫描 GPIO1~GPIO14（覆盖 Arduino Nano ESP32 所有模拟引脚候选）
// ============================================================

// 待扫描的 GPIO 列表（ESP32-S3 上可能的 ADC 引脚）
static const uint8_t kScanPins[] = {
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14,
    15, 16, 17, 18, 45, 46, 47, 48
};
static const uint8_t kPinCount = sizeof(kScanPins) / sizeof(kScanPins[0]);

void setup() {
    Serial.begin(115200);
    delay(1500);

    Serial.println("\n========================================");
    Serial.println("  GPIO 扫描诊断 — 找出 A2 真实 GPIO");
    Serial.println("========================================");
    Serial.println("请确保扩展板 A2 已用杜邦线接到 GND");
    Serial.println("读数接近 0 的 GPIO 就是 A2 的真实引脚号\n");

    analogReadResolution(12);

    for (uint8_t i = 0; i < kPinCount; i++) {
        pinMode(kScanPins[i], INPUT);
    }
}

void loop() {
    Serial.println("--- GPIO ADC Scan ---");
    Serial.println("GPIO | raw  | volts | 状态");
    Serial.println("-----+------+-------+---------");

    for (uint8_t i = 0; i < kPinCount; i++) {
        uint8_t gpio = kScanPins[i];

        // 过采样 4 次取均值
        uint32_t acc = 0;
        for (int j = 0; j < 4; j++) {
            int val = analogRead(gpio);
            if (val >= 0) acc += val;
        }
        uint16_t raw = (uint16_t)(acc / 4);
        float volts = raw * 3.3f / 4095.0f;

        // 状态判断
        const char* status;
        if (raw < 50) {
            status = "<<<< GND! 这个可能是 A2";
        } else if (raw > 4000) {
            status = "接近 VCC";
        } else if (raw > 300 && raw < 700) {
            status = "浮空中（可能是当前读的引脚）";
        } else {
            status = "";
        }

        Serial.printf("  %2u | %4u | %.2fV | %s\n",
                      gpio, raw, (double)volts, status);
    }

    Serial.println("");
    delay(2000);
}
