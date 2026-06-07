# test_mic_capture — INMP441 麦克风硬件验证

独立 sketch，用于在接入主固件前**单独验证 INMP441 数字麦克风**的接线、I2S 配置与
真实 PCM 数据采集是否正常。**不嵌入主程序 `LingxiGlove_Main/`**。

> 调试该 sketch 走通后，主固件 `mic_capture.cpp` 才能可靠工作。

---

## 1. 硬件接线

| INMP441 焊盘 | Arduino Nano ESP32 排针 | 实际 GPIO | 说明 |
|---|---|---|---|
| **VDD** | **3V3** | — | ⚠️ **严禁接 5V**（INMP441 上限 3.63V，5V 直接击穿） |
| GND  | GND  | — | 必须与板子共地 |
| **L/R** | **GND** | — | 接地 → 麦克风在 I²S 帧的【左声道时隙】输出 |
| SCK  | D10  | GPIO21 | I²S BCLK |
| WS   | D11  | GPIO38 | I²S LRCLK |
| SD   | D12  | GPIO47 | I²S DIN（PCM 数据） |

> Arduino IDE 必须设为 **Pin Numbering = By Arduino pin (default)**。
> ESP-IDF 直接 API（如 `i2s_set_pin`）**不参与 Arduino pin remap**，sketch 内部用
> `digitalPinToGPIONumber()` 把 D10/D11/D12 转换成真实 GPIO 21/38/47。

---

## 2. I²S 关键配置

| 参数 | 取值 | 原因 |
|---|---|---|
| `bits_per_sample` | `I2S_BITS_PER_SAMPLE_32BIT` | INMP441 输出 24-bit 左对齐，必须 32-bit 容器承载 |
| `channel_format` | `I2S_CHANNEL_FMT_ONLY_LEFT` | L/R 接 GND → 数据落在左声道时隙 |
| `communication_format` | `I2S_COMM_FORMAT_STAND_I2S` | 标准 I²S Philips 格式 |
| 数据下变频 | `int16 = (int32 raw) >> 14` | 提取 24-bit 数据的高 16 位 |

⚠️ 误配 `channel_format` 的典型故障：
- L/R=GND 但配成 ONLY_RIGHT  → ESP32 在右声道时隙读到 INMP441 的 **Hi-Z**
  - 若内部弱上拉生效：raw 全 `0xFFFFFFFF`，RMS≈1
  - 若无上下拉：raw 持续 `0x00000000`，RMS=0（**易被误判为模块没上电**）

---

## 3. 烧录与运行

1. Arduino IDE 选择 board: `Arduino Nano ESP32`
2. **Pin Numbering** → `By Arduino pin (default)`
3. 打开 `test_mic_capture.ino`，烧录
4. 打开串口监视器（115200 baud）

---

## 4. 串口输出判读黄金标准

正常输出形如：

```
[PIN] D10 -> Arduino pin=10, real GPIO=21 (BCLK / SCK)
[PIN] D11 -> Arduino pin=11, real GPIO=38 (LRCLK / WS)
[PIN] D12 -> Arduino pin=12, real GPIO=47 (DIN / SD)

---- ESP32 内部 GPIO 自检 ----
[SELFTEST] D10/BCLK drive_high_read=1 drive_low_read=0  -> OK
[SELFTEST] D11/WS   drive_high_read=1 drive_low_read=0  -> OK
[SELFTEST] D12/SD   drive_high_read=1 drive_low_read=0  -> OK

[MIC] I2S1 初始化成功 (16kHz/16-bit/mono)
[MIC] RMS=8292  peak16=9091  rawPeak=0x08e09640  ████████████████   ← 启动瞬间正常
[MIC] RMS=21    peak16=56    rawPeak=0x000de000  ░░░░░░░░░░░░░░░░   ← 安静本底
[MIC] RMS=1398  peak16=3275  rawPeak=0x0332a000  ██░░░░░░░░░░░░░░   ← 说话响应
[RAW] ff175200 ff102400 fef94a00 fee3b600 ...                       ← 真实 PCM
```

### 判读阈值

| 指标 | 期望范围 | 含义 |
|---|---|---|
| 启动首条 RMS | 几千到上万 | DMA 冷启动残留，**正常一过性**，可忽略 |
| 安静环境 RMS | 20~500 | I²S 链路干净，本底噪声 |
| 说话/拍手 RMS | > 800 | 麦克风正确响应声学事件 |
| RAW 形态 | 正负交替的 `ff..` / `00..`（24-bit 左对齐） | 真实 PCM |
| RAW 全 `0x00000000` 或 `0xFFFFFFFF` 不变 | ❌ | 见下表故障树 |

---

## 5. 故障定位表

| 现象 | 最可能原因 | 处理 |
|---|---|---|
| RMS=1，rawPeak=`0xFFFFFFFF` | `channel_format` 与 L/R 物理电平反了；或 D10/11/12 没用 `digitalPinToGPIONumber()` 转 GPIO | 切换 `ONLY_LEFT`/`ONLY_RIGHT`；确认 sketch 用了 GPIO 转换 |
| RMS=0，rawPeak=`0x00000000` | 同上（无内部上拉时表现）；或 INMP441 真没上电 | **优先**先切对 channel_format；仍不行再查 VDD |
| `[SELFTEST]` 报 BAD | ESP32 该 GPIO 物理损坏 | 改用其他空闲 GPIO，更新 sketch 与主固件 |
| `[PROBE]` D12 显示 FLOATING + `[COUPLE]` t+5us=0 | 大概率 channel_format 反，**不要**轻易判定为没上电 | 先切 channel_format，再考虑硬件 |
| `[PIN]` 显示 D10→GPIO10 不是 21 | Arduino IDE 的 Pin Numbering 设成了 "By GPIO number" | 改回 "By Arduino pin (default)" |

---

## 6. 验证通过后

1. 主固件 `LingxiGlove_Main/config.h` 设置 `ENABLE_MIC_CAPTURE = 1`
2. 主固件 `mic_capture.cpp` 已使用相同配置（32-bit、ONLY_LEFT、`digitalPinToGPIONumber()`）
3. 此 sketch 保留作为回归工具，硬件改动后跑一次即可定位是否软件问题
