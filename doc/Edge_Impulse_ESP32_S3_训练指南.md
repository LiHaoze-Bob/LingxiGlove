# Edge Impulse 模型训练指南（端到端路线，ESP32-S3 部署版）

**适用项目**：灵犀手套 LingxiGlove — 手语翻译手套
**目标平台**：Arduino Nano ESP32（ESP32-S3 @ 240MHz）
**技术路线**：端到端 1D CNN / Dense（信号 → 手语词，神经网络隐层自动学组合）
**两阶段策略**：阶段 1 单路 flex 流水线压测 → 阶段 2 五路 flex + IMU 真手语训练

---

## 0. 路线决策与适用范围

### 0.1 唯一路线：端到端模型

本项目统一采用 **端到端路线**：
- 模型输入：原始多维传感器信号（flex 归一化 + IMU 加速度 + 陀螺仪）
- 模型输出：直接是手语词类别（softmax 概率向量）
- "5 指弯曲组合 → 手语" 的映射由神经网络**隐层自动学习**，**不手写规则表**

**不采用** 「基元识别 + 规则查找」的分层方案，原因：
1. 真手语含手腕朝向、轨迹、双手协同，规则枚举不完整
2. 加新手语只需重采 + 重训，无需修改 C++ 规则代码
3. EI Studio 工作流天然契合端到端范式
4. 准确率上限显著高于规则法

### 0.2 两阶段演进

| 阶段 | sensor 数量 | 训练目标 | 数据用途 |
|------|-------------|----------|----------|
| **阶段 1（当前）** | 1 路 flex（食指） | 3 类静态：straight / half / full | 流水线压测，**不用于真手语推理** |
| **阶段 2（5 路到货后）** | 5 路 flex + 3 维 IMU | 10-20 个高频手语词 | 真手语翻译，端侧直接播报 |

阶段 1 产物（3 类模型）在阶段 2 全部作废，仅保留**工程链路与脚本**复用。

### 0.3 与现有代码的对接点

| 模块 | 路径 | 用途 |
|------|------|------|
| 采集协议 | [LingxiGlove_Main.ino](../src/LingxiGlove_Main/LingxiGlove_Main.ino) MODE_CAPTURE | 串口 CSV + 数字键打标 |
| PC 抓包 | [tools/capture_serial.py](../tools/capture_serial.py) | 落盘 raw.csv |
| 数据集构建 | [tools/build_dataset.py](../tools/build_dataset.py) | 切窗 + EI CSV + npy |
| 识别器抽象 | [gesture_recognizer.h](../src/LingxiGlove_Main/gesture_recognizer.h) | 工厂 RuleBased / EdgeImpulse |
| EI 后端 | [edge_impulse_recognizer.h](../src/LingxiGlove_Main/edge_impulse_recognizer.h) | EI 库调用封装 |
| 后端切换宏 | [config.h](../src/LingxiGlove_Main/config.h) `RECOGNIZER_BACKEND` | RULE / EDGE_IMPULSE |

---

# 阶段 1：单路 flex 流水线验证（当前阶段）

> 目标：用 1 路 flex + 3 类静态姿态，跑通「采集 → 切窗 → 训练 → 部署 → 评测」全链路。
> 模型本身不是真手语，**不要把 3 类映射成"你好/再见"等语义**。

## 1. 准备工作

### 1.1 注册 Edge Impulse 账号

1. 访问 https://studio.edgeimpulse.com
2. 邮箱注册免费账号（Personal Project 即可）
3. 新建项目 `LingxiGlove-Gesture-Recognition`

### 1.2 硬件准备

确认以下已通过验证：

| 项 | 要求 | 验证脚本 |
|----|------|----------|
| Flex 接线 | 左手食指 → 扩展板 A2（GPIO3） | [test_flex_read.ino](../src/tests/test_flex_read/test_flex_read.ino) |
| Flex 校准 | FLEX_ADC_MIN=1100, MAX=2800 | 串口 CSV 看 flexNorm 范围 |
| Arduino IDE | 板子选 `Arduino Nano ESP32`，Pin Numbering = `By Arduino pin (default)` | — |

### 1.3 ESP-NN 优化（自动启用，无需配置）

EI 导出的 Arduino library 已内嵌 ESP-NN（ESP32-S3 LX7 SIMD 加速），编译时通过宏 `ESP_NN` + `ESP32` 自动启用。**Tools 菜单里没有 ESP-NN 开关，无需手动配置**。

验证启用成功：导入库后烧录，串口看到 `Classification: <5 ms` 说明走了优化路径。

---

## 2. 数据采集（端侧串口 CSV + 实时打标）

### 2.1 协议总览

```
端侧 MODE_CAPTURE
   ├── CSV 表头：timestamp_ms,ax,ay,az,gx,gy,gz,pitch,roll[,flex0..,flexNorm0..],label
   ├── 每帧追加一行（采样间隔 SENSOR_READ_INTERVAL = 50ms → 20Hz）
   └── 串口键入数字键 0/1/2 切换 label，'-' 复位为 unlabeled(-1)

PC 端 capture_serial.py
   ├── 监听串口，自动建 output/capture/session_<时间戳>/raw.csv
   ├── 跳过 '[' 开头的日志行
   └── Ctrl+C 收尾时按 label 统计行数
```

### 2.2 采集步骤

**Step 1 — 烧录并进入采集模式**

```
1. Arduino IDE 烧录 LingxiGlove_Main.ino
2. 打开串口监视器（115200）
3. 发送字符 'c' → 进入 MODE_CAPTURE
4. 应看到提示：
   [capture] mode entered. label=-1(unlabeled).
   Press 0/1/2 to set label, '-' to reset, 'r' to leave.
   timestamp_ms,ax,...,roll,flex0,...,flexNorm4,label
```

**Step 2 — PC 端开抓包**

```bash
cd /Users/kun.li/Code/Lingxi/LingxiGlove
python tools/capture_serial.py --port auto --baud 115200
```

**Step 3 — 边戴手套边按数字键打标**

| 串口键 | label | 动作 | 持续时间 |
|--------|-------|------|----------|
| `0` | straight | 食指完全伸直，保持静止 | ≥ 30 s |
| `1` | half | 半弯（约 45°），保持静止 | ≥ 30 s |
| `2` | full | 完全握拳，保持静止 | ≥ 30 s |
| `-` | -1 | 动作切换过渡阶段 | 1-2 s |

**关键技巧**：每次切动作前**先按 `-`**，等手势稳定后再按对应数字键，过渡帧会被丢弃。

**Step 4 — 收尾**

PC 端按 `Ctrl+C`，应看到统计：

```
label=0 (straight) : 1480 rows
label=1 (half)     : 1502 rows
label=2 (full)     : 1455 rows
label=-1 (skipped) : 320 rows
```

三类均衡（相差 < 20%）才算合格。**至少 3 次完整会话**（不同时段、不同手套松紧），让模型见过分布漂移。

---

## 3. 数据集构建

```bash
python tools/build_dataset.py \
    --in output/capture \
    --window 20 \
    --stride 10 \
    --flex-channel 1 \
    --test-ratio 0.2
```

参数说明：
- `--window 20`：1 秒窗口（20 帧 × 50ms）
- `--stride 10`：50% 重叠
- `--flex-channel 1`：取食指通道（index=1，对应 FLEX_PIN_INDEX）
- `--test-ratio 0.2`：8:2 训练 / 测试切分（按 label 分层）

输出：

```
output/dataset/
├── ei_csv/
│   ├── train/   straight.0000.csv  half.0000.csv  full.0000.csv  ...
│   └── test/    （同结构）
├── X_train.npy  y_train.npy
└── X_test.npy   y_test.npy
```

每个 EI CSV 文件 21 行（header + 20 帧），列固定为 `timestamp,flex`。

---

## 4. Edge Impulse Studio 配置（阶段 1 最小验证版）

### 4.1 上传数据

1. 左侧 **Data acquisition** → 右上 **Upload data**
2. Upload mode: `Select files`
3. Files: 选 `output/dataset/ei_csv/train/` 下所有 csv（可多选）
4. Category: `Training`
5. Label: 勾选 **Infer from filename**（EI 从 `straight.0000.csv` 解析出 label）
6. 重复上述，把 `test/` 下文件传到 `Testing` 类别

### 4.2 配置 Impulse

`Impulse design → Create impulse`：

| 模块 | 参数 |
|------|------|
| Time series data | Window size = **1000 ms**，Window increase = **1000 ms**（CSV 已切窗，不要二次切） |
| | Frequency = **20 Hz** |
| | Zero-pad: 勾选 |
| Processing block | **Flatten**（仅勾 `flex` axis，统计量 Min/Max/Mean/RMS/StdDev 全选） |
| Learning block | **Classification (Keras)** |

点 **Save Impulse**。

### 4.3 生成特征

1. 左侧 **Flatten** → `Save parameters` → `Generate features`
2. **Feature explorer** 查看：三类应聚成 3 个明显分离的簇
3. 若混在一起：重新采集（检查传感器松动、校准值、手势稳定度）

### 4.4 训练分类器

`Classifier` 标签页：

```
Dense(16, ReLU) → Dropout(0.2) → Dense(3, softmax)
epochs = 60
learning rate = 0.001
batch = 16
validation split = 20%
✅ Quantized (int8)
```

点 `Start training`。准确率目标 ≥ 95%（3 类静态极易达到）。

---

## 5. 端侧部署与评测

### 5.1 导出 Arduino library

1. `Deployment` → 选 **Arduino library**
2. ✅ Quantized (int8)
3. Target: `Espressif ESP-EYE (ESP32 240MHz)`（与 ESP32-S3 兼容）
4. `Build` → 下载 zip 到 `LingxiGlove/output/ei_lib/`

### 5.2 导入并切换后端

```
1. Arduino IDE → 项目 → 加载库 → 添加 .ZIP 库 → 选 output/ei_lib/*.zip
2. 编辑 src/LingxiGlove_Main/edge_impulse_recognizer.cpp 顶部
   #include 与 EI 库名一致，如：
   #include <LingxiGlove-Gesture-Recognition_inferencing.h>
3. config.h 把 RECOGNIZER_BACKEND 改为 RECOGNIZER_BACKEND_EDGE_IMPULSE
4. 重新编译烧录
```

串口启动横幅应看到 `[系统] 手势识别器就绪: EdgeImpulse(Flex)`。

做 straight / half / full 三种姿势，应分别命中并播报 `伸直 / 半弯 / 握拳`。

回滚很容易：把宏改回 `RECOGNIZER_BACKEND_RULE` 即可恢复 MPU6050 角度规则识别。

### 5.3 双端对齐评测

**端侧**（结果写入 LittleFS，串口取回）：

```
test 11 30    # straight 30 次
test 12 30    # half 30 次
test 13 30    # full 30 次
test export   # dump 所有日志到串口
```

**PC 端复跑同一模型，与端侧结果对账**：

```bash
python tools/eval_offline.py \
    --model output/ei_lib/<你的库>.zip \
    --testset output/dataset
```

输出：

```
output/eval/
├── confusion_matrix.png
└── report.txt        per-class precision/recall + 平均推理延迟 + 模型大小
```

**验收**：3 类总 accuracy ≥ 95%，端侧与 PC 端差异 ≤ 1%。

---

# 阶段 2：真手语端到端训练（5 路 sensor 到货后）

> 目标：用 5 路 flex + IMU 8 维输入，端到端训练一个能识别真手语词的模型。
> 阶段 1 的工程链路（采集脚本、build_dataset、EI 上传、端侧推理框架）**全部复用**，
> 唯一改变的是「数据集内容」和「模型架构」。

## 6. 手语词列表设计

### 6.1 首批 10 个高频词（建议）

| ID | 手语词 | 类型 | 备注 |
|----|--------|------|------|
| 0 | 你好 | 动态 | 摆手 |
| 1 | 谢谢 | 动态 | 拇指轻拍下巴前移 |
| 2 | 再见 | 动态 | 挥手 |
| 3 | 对不起 | 动态 | 握拳画圈 |
| 4 | 是 / 好 | 静态 | 拇指竖起 |
| 5 | 不 | 静态 | 食指中指并拢摇晃 |
| 6 | 帮助 | 静态 | 一手握拳放另一手掌上 |
| 7 | 吃饭 | 动态 | 食中指向嘴部 |
| 8 | 喝水 | 动态 | 拇指模拟杯口 |
| 9 | 厕所 | 静态 | 拇指夹于食中指间 |

### 6.2 标签命名规范

- 全部小写英文（EI Studio 限制）：`hello / thanks / bye / sorry / yes / no / help / eat / drink / toilet`
- 与端侧 `CAPTURE_LABEL_NAMES[]` 一一对应
- `CAPTURE_LABEL_COUNT` 改为 `10`

---

## 7. 数据采集策略

### 7.1 采集规范

| 参数 | 设置 | 说明 |
|------|------|------|
| 采样率 | 20 Hz | 端侧主循环 SENSOR_READ_INTERVAL=50ms |
| 时间窗口 | **2 秒 = 40 帧** | 覆盖动态手势完整动作 |
| 滑窗步长 | 1 秒 = 20 帧 | 50% 重叠 |
| 每词样本数 | **每人 30 次 × 3 人 = 90 条** | 覆盖个体差异、力度、速度 |
| 负样本 | 占总量 10% | 随机摆动 / 无意义手势，label = `noise` |
| 采集环境 | 室内自然光 | 避免极端温度影响传感器 |

### 7.2 采集流程

**Step 1 — 升级 sketch（10 类 label）**

修改 `config.h`：

```c
#define CAPTURE_LABEL_COUNT     10  // 或 11（含 noise）
```

修改 `LingxiGlove_Main.ino` 中 `CAPTURE_LABEL_NAMES[]`：

```c
static const char* const CAPTURE_LABEL_NAMES[CAPTURE_LABEL_COUNT] = {
    "hello", "thanks", "bye", "sorry", "yes",
    "no", "help", "eat", "drink", "toilet"
};
```

**Step 2 — 按词分会话采集**

```
0 → 完整做 1 次"你好"动作（约 1.5-2 秒）→ -
0 → 再做 1 次"你好" → -
... 重复 30 次
按 1 切到"谢谢"，重复 30 次
... 直到全部 10 词
```

**关键技巧**：
- 每个动作前后**留 200-500ms 静止**，避免动作连贯混淆
- 每次都按 `-` 复位再按数字键，保证只有动作核心帧被打标
- 同一词的 30 次要**有意做轻微变化**（力度、速度、手腕角度），让模型学到鲁棒性

**Step 3 — PC 端抓包**

```bash
python tools/capture_serial.py --port auto
# 至少 3 个人各采一轮，目录会自动按 session 分开
```

### 7.3 数据集构建

```bash
# 升级 build_dataset.py 调用，支持多通道 + 长窗
python tools/build_dataset.py \
    --in output/capture \
    --window 40 \
    --stride 20 \
    --multi-channel \
    --test-ratio 0.15
```

> `--multi-channel` 后续需要在脚本中开放支持多列输出（当前脚本是单通道版），sensor 到货后再升级一次。

每条 EI CSV 文件 41 行（header + 40 帧），列扩展为：
```
timestamp,flex0,flex1,flex2,flex3,flex4,accX,accY,accZ,gyroX,gyroY,gyroZ
```

---

## 8. Edge Impulse Studio 配置（阶段 2）

### 8.1 项目设置

新建独立项目 `LingxiGlove-Sign-10Class`（与阶段 1 项目区分），避免数据混淆。

### 8.2 Impulse design

| 模块 | 参数 |
|------|------|
| Time series data | Window size = **2000 ms**，Window increase = **1000 ms** |
| | Frequency = **20 Hz** |
| | Axes：勾选全部 11 维 |
| Processing block | **Spectral Analysis**（FFT + RMS，动态手势更适合频域） + **Flatten**（静态特征兜底） |
| Learning block | **Classification (Keras)** |

### 8.3 生成特征

`Spectral features → Save parameters → Generate features` → `Feature explorer` 检查 10 类分离度。

不同类应可视化区分；若严重重叠，回去补采样。

---

## 9. 模型训练（1D CNN）

### 9.1 Expert mode 网络架构

`Classifier` 标签页 → `Switch to Keras (expert mode)`：

```python
import tensorflow as tf
from tensorflow.keras.models import Sequential
from tensorflow.keras.layers import (
    Dense, Conv1D, Dropout, Reshape, GlobalAveragePooling1D
)

# 输入形状由 EI 自动注入：input_length = 40 * 11 = 440
model = Sequential()

# 还原时间维度：(timesteps=40, features=11)
model.add(Reshape((40, 11), input_shape=(input_length,)))

# Conv1D 块 1：局部时序特征
model.add(Conv1D(32, kernel_size=3, activation='relu', padding='same'))
model.add(Dropout(0.2))

# Conv1D 块 2：更高阶时序模式
model.add(Conv1D(64, kernel_size=3, activation='relu', padding='same'))
model.add(Dropout(0.2))

# Global Average Pooling 替代 Flatten，参数量减少 90%
model.add(GlobalAveragePooling1D())

# Dense 分类头
model.add(Dense(32, activation='relu'))
model.add(Dropout(0.3))
model.add(Dense(classes, activation='softmax'))
```

### 9.2 训练参数

| 参数 | 值 | 说明 |
|------|-----|------|
| Epochs | 100 | 早停回调可自动停止 |
| Learning rate | 0.001 | Adam 优化器 |
| Batch size | 32 | 数据量大可上 64 |
| Validation split | 20% | 从 Training 自动切 |

### 9.3 数据增强（强烈推荐）

`Data augmentation` 区域：

- **Add noise**：高斯噪声幅度 0.02（模拟 ADC 抖动）
- **Time warping**：5% 时间轴拉伸（模拟动作快慢）
- **Magnitude warping**：5% 幅度缩放（模拟传感器漂移）

不要超过 10%，否则会破坏手势本质。

### 9.4 训练目标

| 指标 | 目标 | 达不到怎么办 |
|------|------|--------------|
| Training accuracy | ≥ 95% | 增加 epochs 或调大网络 |
| Validation accuracy | ≥ 90% | 增加数据增强或正则化 |
| Train/Val gap | ≤ 5% | 提高 Dropout 到 0.4 |
| 单类 recall | ≥ 85% | 针对该类补采样 |

### 9.5 混淆矩阵分析

`Model performance` 看混淆矩阵，常见易混淆对及解决：

| 易混淆对 | 原因 | 对策 |
|----------|------|------|
| eat vs drink | 都是手部贴近脸部 | 强化 IMU pitch 区分（吃饭手心朝下，喝水手心朝侧） |
| yes vs no | 手指弯曲度接近 | 加大窗口到 60 帧捕获晃动节奏 |
| hello vs bye | 都是挥手动作 | 增加 30% 样本，让模型学到方向差异 |

---

## 10. 端侧部署与连续识别

### 10.1 导出与切换

阶段 2 的部署流程**与阶段 1 完全一致**（见 §5.1-5.2），只换 EI 库 zip 包。

```c
// edge_impulse_recognizer.cpp 顶部
#include <LingxiGlove-Sign-10Class_inferencing.h>

// gesture_recognizer.h 同步扩 GestureType 枚举到 10 项
GESTURE_SIGN_HELLO, GESTURE_SIGN_THANKS, ..., GESTURE_SIGN_TOILET,
```

### 10.2 连续识别：滑窗投票防抖

单次推理易受噪声影响，端侧加投票机制：

```cpp
// edge_impulse_recognizer.cpp
const int VOTE_WINDOW    = 10;   // 最近 10 次推理结果
const int VOTE_THRESHOLD = 7;    // 同一类 ≥ 7 次才确认
const float CONF_GATE    = 0.70f;

GestureType vote_buffer[VOTE_WINDOW] = {GESTURE_NONE};
int vote_ix = 0;

GestureType voted_predict(GestureType raw, float confidence) {
    if (confidence < CONF_GATE) raw = GESTURE_NONE;  // 低置信度不入票
    vote_buffer[vote_ix] = raw;
    vote_ix = (vote_ix + 1) % VOTE_WINDOW;

    int counts[GESTURE_COUNT] = {0};
    for (int i = 0; i < VOTE_WINDOW; i++) counts[vote_buffer[i]]++;

    for (int g = 0; g < GESTURE_COUNT; g++) {
        if (counts[g] >= VOTE_THRESHOLD) return (GestureType)g;
    }
    return GESTURE_NONE;
}
```

### 10.3 推理代码骨架

```cpp
#include <LingxiGlove-Sign-10Class_inferencing.h>

// 环形缓冲：40 帧 × 11 维 = 440 浮点
float feature_buf[EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE];
size_t write_ix = 0;
size_t frames_since_infer = 0;

GestureResult EdgeImpulseRecognizer::recognize(const SensorData& data) {
    // 1. 拼当前帧（顺序必须与训练时 CSV 列顺序一致）
    float frame[11] = {
        data.flexNorm[0], data.flexNorm[1], data.flexNorm[2],
        data.flexNorm[3], data.flexNorm[4],
        data.accelX / 16384.0f, data.accelY / 16384.0f, data.accelZ / 16384.0f,
        data.gyroX  /   131.0f, data.gyroY  /   131.0f, data.gyroZ  /   131.0f
    };

    // 2. 推入环形缓冲
    for (int i = 0; i < 11; i++) {
        feature_buf[write_ix * 11 + i] = frame[i];
    }
    write_ix = (write_ix + 1) % EI_CLASSIFIER_RAW_SAMPLE_COUNT;
    frames_since_infer++;

    // 3. 每 EI_INFERENCE_STRIDE_FRAMES (5 帧 = 250ms) 推理一次
    if (frames_since_infer < EI_INFERENCE_STRIDE_FRAMES) {
        return { GESTURE_NONE, 0.0f, "" };
    }
    frames_since_infer = 0;

    // 4. 调 EI 推理
    signal_t signal;
    numpy::signal_from_buffer(feature_buf,
                              EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE, &signal);
    ei_impulse_result_t result = {0};
    if (run_classifier(&signal, &result, false) != EI_IMPULSE_OK) {
        return { GESTURE_NONE, 0.0f, "" };
    }

    // 5. argmax + 置信度阈值
    float max_v = 0; int max_i = -1;
    for (size_t i = 0; i < EI_CLASSIFIER_LABEL_COUNT; i++) {
        if (result.classification[i].value > max_v) {
            max_v = result.classification[i].value;
            max_i = i;
        }
    }
    GestureType raw = label_to_gesture(
        ei_classifier_inferencing_categories[max_i]);

    // 6. 投票防抖
    GestureType voted = voted_predict(raw, max_v);
    return { voted, max_v, s_gestureTexts[voted] };
}
```

### 10.4 端侧资源占用预算

| 指标 | 目标（阶段 2） | ESP32-S3 容量 | 余量 |
|------|----------------|---------------|------|
| 推理延迟 | ≤ 30 ms | (主循环 50 ms) | 40% |
| Flash | ≤ 200 KB | 8 MB | >97% |
| RAM | ≤ 40 KB | 512 KB | >92% |

---

## 11. 模型优化技巧

### 11.1 Int8 量化对比

| 指标 | FP32 | Int8 | 损失 |
|------|------|------|------|
| 模型体积 | ~180 KB | ~50 KB | ↓ 72% |
| 推理延迟 | ~25 ms | ~12 ms | ↓ 52% |
| 准确率 | 92.3% | 91.0% | ↓ 1.3% |
| RAM | ~30 KB | ~15 KB | ↓ 50% |

只要 Int8 准确率损失 < 3%，**默认开启 Int8**。

### 11.2 EON Compiler

EI Studio `Deployment → EON Compiler` 选 `Enabled`，可在不损失精度的前提下进一步压缩 25-50% RAM 占用。免费版可用。

### 11.3 内存不足时的降级路径

| 症状 | 降级措施 |
|------|----------|
| Heap allocation failed | 减小 Batch / 关闭其它任务的大缓冲 |
| Flash 超 1MB | Conv1D 滤波器减半（32→16, 64→32） |
| 延迟 > 50ms | 时间窗口缩到 30 帧；或 Frequency 降到 10Hz |
| RAM 紧张 | 在 `sdkconfig` 启用 PSRAM 存特征缓冲 |

---

# 附录

## A.1 常见问题排查

| 问题 | 可能原因 | 解决方案 |
|------|----------|----------|
| 推理结果全为同一类别 | 输入数据未归一化 | 检查端侧 flexNorm 计算与训练 CSV 是否一致 |
| 现场识别远低于 EI Test accuracy | 训练数据与佩戴方式不同 | 增加不同人 / 不同松紧度样本 |
| 模型体积超过预算 | 网络太复杂 / 未开 Int8 | 精简 Conv 层；确认 Quantized 已勾选 |
| 推理延迟超过 50ms | 网络太深 / 窗口太大 | 降低滤波器数 / 缩短窗口 |
| 某些手势始终识别不对 | 特征区分度不够 | 看混淆矩阵，针对性补该手势样本 |
| EI Studio 上传失败 | 文件名 label 推断失败 | 文件名必须 `<label>.<seq>.csv` 格式 |
| 端侧 / PC 评测结果差异 > 5% | 特征拼装顺序不一致 | 核对端侧 frame[] 与 CSV 列顺序逐位对齐 |
| Arduino IDE 编译报 `esp_nn` 找不到 | 板子未选 Nano ESP32 | Tools → Board → Arduino Nano ESP32 |
| ENABLE_FLEX_SENSORS=1 编译报 #error | 引脚或校准未定义 | 见 [config.h §7](../src/LingxiGlove_Main/config.h) |
| `analogRead(3)` 读到固定值 | Pin Numbering 模式不一致 | 必须用 `A2` 常量，不要硬编码 GPIO |

## A.2 模型升级流程（加新手语词）

```
1. config.h: CAPTURE_LABEL_COUNT += 1
2. LingxiGlove_Main.ino: CAPTURE_LABEL_NAMES[] 末尾追加新词
3. gesture_recognizer.h: GestureType 枚举末尾追加 GESTURE_SIGN_<NEW>
4. gesture_recognizer.cpp: s_gestureTexts[] 末尾追加文本
5. edge_impulse_recognizer.cpp: label_to_gesture() 加映射分支
6. 重新烧录采集 → 跑 capture_serial → build_dataset
7. EI Studio 上传新数据 → 重训 → 重导出 .zip
8. Arduino IDE 重新导入 zip → 烧录验证
```

整个流程 **无需改动核心架构**（识别器抽象、TTS 链路、ESP-NOW 协议都不变）。

## A.3 参考资源

- Edge Impulse 官方文档：https://docs.edgeimpulse.com
- Edge Impulse ESP32 部署指南：https://docs.edgeimpulse.com/docs/edge-ai-hardware/cpu/espressif-esp32
- Arduino Nano ESP32 指南：https://docs.arduino.cc/hardware/nano-esp32
- ESP-NN（LX7 SIMD）：https://github.com/espressif/esp-nn
- 项目参考论文：London Metropolitan University, *Sign Language Detection using Smart Glove*, 2023

---

## A.4 阶段 1 → 阶段 2 检查清单

阶段 1 完成（流水线跑通）后，启动阶段 2 前确认：

- [ ] 5 路 flex 全部到货并完成硬件接线 + 校准
- [ ] `FLEX_PIN_*` 全部从占位改为实际引脚
- [ ] `FLEX_ADC_MIN/MAX` 用 `k` 校准命令重新写入
- [ ] `CAPTURE_LABEL_COUNT` 改为目标手语词数（如 10）
- [ ] `CAPTURE_LABEL_NAMES[]` 同步更新
- [ ] `build_dataset.py` 升级到 multi-channel 输出
- [ ] EI 新建独立项目，避免与阶段 1 数据混淆
- [ ] 至少 3 人各采 1 轮，每词 30 条样本
- [ ] 网络架构升级为 1D CNN（Conv1D × 2 + GAP + Dense）
- [ ] 端侧加滑窗投票防抖（VOTE_WINDOW=10, THRESHOLD=7）

完成上述全部后，端侧应能播报真手语词，进入 demo 演示阶段。
