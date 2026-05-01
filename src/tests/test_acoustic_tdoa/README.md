# test_acoustic_tdoa — D2: 声学 TDOA 测距 POC

双手手语翻译方案中"用超声 chirp 做相对距离估计"的真机可行性验证。
本目录只做 POC，**不嵌入主程序 `LingxiGlove_Main/`**。

## 1. 目录文件

| 文件 | 说明 |
|---|---|
| `test_acoustic_tdoa.ino` | 单 sketch，用 `ROLE_TX` / `ROLE_RX` 编译开关切换发端/收端 |
| `chirp_pcm.h` | 由 `tools/gen_chirp_pcm.py` 生成的 17–19 kHz / 48 kHz / 5 ms chirp int16 模板 |

配套工具：`tools/gen_chirp_pcm.py`（项目根下）—— 重新生成 `chirp_pcm.h`。

## 2. 硬件需求

- **两块** ESP32-S3 开发板（Arduino Nano ESP32 / ESP32-S3-DevKit 均可）
- 发端用 MAX98357A + 小喇叭
- 收端用 INMP441 I2S 数字麦克风
- 可选：示波器（观察 `TRIG_PIN` 与实际声波之间的端到端延迟）

## 3. 接线（与仓库 `config.h` 保持一致）

### 3.1 TX（发端）板
```
MAX98357A:
  BCLK -> GPIO7   (D4)
  LRC  -> GPIO8   (D5)
  DIN  -> GPIO9   (D6)
  SD   -> GND   (右声道 mono)
  VCC  -> 3V3 或 5V
  GND  -> GND
  喇叭 -> MAX98357A 绿色接线柱

TRIG（可选，接示波器）:
  GPIO6 (D3) -> 示波器 CH1
```

### 3.2 RX（收端）板
```
INMP441:
  SCK -> GPIO15  (D10)
  WS  -> GPIO16  (D11)
  SD  -> GPIO17  (D12)
  L/R -> GND     (选左声道)
  VCC -> 3V3
  GND -> GND
```

GPIO 15/16/17 是本 POC 默认选择，`LingxiGlove_Main/` 主程序未占用这三
个口。如果你的板子上已经接了别的外设，改 `.ino` 里
`kRxBclkPin/kRxWsPin/kRxDataPin` 即可。

## 4. 编译与烧录

### 4.1 Arduino IDE
1. 板子选 **Arduino Nano ESP32** 或 **ESP32S3 Dev Module**
2. 打开 `test_acoustic_tdoa.ino`
3. 文件头部保留 `#define ROLE_TX`（注释掉 `ROLE_RX`）→ 烧录到**发端板**
4. 改成 `#define ROLE_RX`（注释掉 `ROLE_TX`）→ 烧录到**收端板**
5. 两块板上电后分别打开各自的串口监视器

> ⚠️ `ROLE_TX` / `ROLE_RX` **必须且只能定义一个**，否则编译会
> 立即以 `#error` 报错（见 ino 头部）。

### 4.2 运行效果（预期）

**TX 串口输出**：
```
================================
 LingxiGlove D2: Acoustic TDOA TX
================================
[TX] chirp: 17000 -> 19000 Hz, 5 ms, 48 kHz
[TX] I2S ready, start emitting chirps
[TX] chirp emitted @ t = 503 ms
[TX] chirp emitted @ t = 1004 ms
...
```

**RX 串口输出**（两手静止相距 ~50 cm 时，每 500 ms 收到一次 chirp）：
```
================================
 LingxiGlove D2: Acoustic TDOA RX
================================
[RX] chirp template: 240 samples @ 48 kHz
[RX] rx window: 1440 samples (~30 ms)
[RX] peak threshold: 20
[RX] ready, listening...
[RX] detect: argmax=72 samples, peak=145M, D_hat=514.6 mm
[RX] detect: argmax=71 samples, peak=139M, D_hat=507.5 mm
[RX] detect: argmax=72 samples, peak=148M, D_hat=514.6 mm
...
```

- `argmax` 是朴素定点相关在 30 ms 窗口里的峰值位置，对应 chirp 在窗口内
  的起始样本号
- `D_hat = argmax / 48000 × 343 × 1000 mm`，直接可读成两手距离
- `peak` 的单位是"百万 int32 乘加累积"；50 cm 距离下典型 ~100M–200M，
  噪声底通常 <10M，所以默认阈值 20M 足以过滤误触发

## 5. 与 D1 仿真的对照

| 维度 | D1 仿真 (`tools/acoustic_tdoa_simulate.py`) | D2 POC（本 sketch） |
|---|---|---|
| chirp 参数 | 48 kHz / 5 ms / 17–19 kHz / Hann 窗 | **完全一致**（`chirp_pcm.h` 直接由同样参数生成） |
| 延迟估计 | 整数样本 argmax（亚样本插值诚实放弃） | 同样整数样本 argmax |
| 距离公式 | `D_hat = k* / fs × c` | 完全一致 |
| 精度下限 | c/fs ≈ 7.15 mm（量化地板） | 真机应收敛到这个下限附近 |
| SNR ≥ 10 dB 时 RMSE | ~0.21 mm (纯量化残差，D=0.5 m) | 预期 ≤ c/fs ≈ 7 mm（离散化下 RMSE 不会小于 1 个样本） |

**验证步骤**（D3 真机实测阶段用）：
1. 两手固定距离 D（尺子量准），按 50 次按钮记录 `argmax`
2. 计算实测 RMSE，对照 D1 表格的对应 SNR 行
3. 若真机 RMSE ≤ 2×c/fs ≈ 15 mm → 方案可行，进入 B 阶段正式白皮书

## 6. 后续可改进项（留给 D3+）

- **同步**：目前 TX/RX 之间没有硬同步，RX 实际算的是"chirp 在窗口内的
  到达时刻"而非"TX 到 RX 的绝对飞行时间"。要测绝对距离需要再加一条
  ESP-NOW 同步脉冲（已有 `esp_now_sync.h/cpp`）或 GPIO 硬触发
- **亚毫米精度**：D1 已实测抛物线亚样本插值对 chirp 有 ~1 样本本质偏置；
  若 D3 真机确认 7 mm 量化地板不够用，需改用 **sinc 插值**或
  **复相关相位法**，届时再更新本 sketch
- **嵌入主程序**：当前 RX 的朴素相关计算 ~3 ms，可以直接作为 25 ms 主循环
  的一个阶段调用；接入主程序时把 `SlidingCorrInt16()` 搬到一个独立模块
  `distance_estimator.h/cpp` 里
