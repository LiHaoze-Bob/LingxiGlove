# Edge Impulse 模型训练指南（ESP32-S3 部署版）

**适用项目**：灵犀手套 LingxiGlove — 手语翻译手套
**目标平台**：Arduino Nano ESP32-S3（ESP32-S3 @ 240MHz）
**模型类型**：1D CNN（时间序列分类）
**输入特征**：11 维（5 弯曲 + 3 加速度 + 3 陀螺仪）
**时间窗口**：20 帧（约 2 秒）
**输出类别**：10 个手势（好、不、谢谢、吃饭、水、我、要、医生、救命、冷）

---

## 一、准备工作

### 1.1 注册 Edge Impulse 账号

1. 访问 https://studio.edgeimpulse.com
2. 用邮箱注册（免费版支持 1 个公开项目，足够竞赛使用）
3. 建议用学校邮箱注册，显得正式

### 1.2 硬件准备

在训练模型之前，你需要先完成数据采集。确保：

- Arduino Nano ESP32-S3 已能正常读取 5 路弯曲传感器（A0-A3, A6）
- MPU6050（I2C A4/A5）能输出加速度和陀螺仪数据
- 串口打印格式统一为 CSV：每行 11 个浮点数，用逗号分隔

**数据采集代码示例（Arduino）：**

```cpp
#include <Wire.h>
#include <MPU6050.h>

MPU6050 mpu;
const int flexPins[5] = {A0, A1, A2, A3, A6}; // 弯曲传感器引脚
float flexMin[5], flexMax[5]; // 校准值

void setup() {
  Serial.begin(115200);
  Wire.begin();
  mpu.initialize();
  
  // 自动校准：伸直3秒记录最小值
  Serial.println("请伸直手指保持3秒...");
  delay(3000);
  for (int i = 0; i < 5; i++) {
    flexMin[i] = analogRead(flexPins[i]);
  }
  
  // 弯曲3秒记录最大值
  Serial.println("请弯曲手指保持3秒...");
  delay(3000);
  for (int i = 0; i < 5; i++) {
    flexMax[i] = analogRead(flexPins[i]);
  }
  
  Serial.println("校准完成，开始采集...");
  Serial.println("timestamp,flex0,flex1,flex2,flex3,flex4,accX,accY,accZ,gyroX,gyroY,gyroZ,label");
}

void loop() {
  // 读取弯曲传感器并归一化
  float features[11];
  for (int i = 0; i < 5; i++) {
    int raw = analogRead(flexPins[i]);
    features[i] = constrain((raw - flexMin[i]) / (flexMax[i] - flexMin[i]), 0.0, 1.0);
  }
  
  // 读取 MPU6050
  int16_t ax, ay, az, gx, gy, gz;
  mpu.getMotion6(&ax, &ay, &az, &gx, &gy, &gz);
  
  // 加速度归一化到 ±2g 范围
  features[5] = ax / 16384.0;
  features[6] = ay / 16384.0;
  features[7] = az / 16384.0;
  
  // 陀螺仪归一化到 ±250°/s 范围
  features[8] = gx / 131.0;
  features[9] = gy / 131.0;
  features[10] = gz / 131.0;
  
  // 串口输出（用于 Edge Impulse Data Forwarder）
  Serial.print(millis());
  for (int i = 0; i < 11; i++) {
    Serial.print(",");
    Serial.print(features[i], 4);
  }
  Serial.println(",unknown"); // label 稍后手动标注
  
  delay(100); // 100Hz 采样率
}
```

---

## 二、数据采集策略

### 2.1 采集规范

| 参数 | 设置 | 说明 |
|------|------|------|
| 采样率 | 100Hz | 每 10ms 采集一帧 |
| 时间窗口 | 2 秒 = 200 帧 | 但模型输入取 20 帧（200ms 滑动窗口，降采样） |
| 每手势采集次数 | 50 次 × 3 人 = 150 次 | 覆盖不同手型、力度、速度 |
| 采集环境 | 室内自然光 | 避免极端温度影响传感器 |

### 2.2 手势采集要点

- **静态手势**（好、我、医生）：保持姿势 2-3 秒，数据较稳定
- **动态手势**（谢谢、不、吃饭、水、救命、冷）：完整做完动作，包含起始→峰值→结束全过程
- **负样本**：随机摆动手臂、无意义手势，帮助模型学习"非目标手势"的拒绝策略

### 2.3 数据上传方式

**方式一：Edge Impulse Data Forwarder（推荐，实时上传）**

1. 安装 Node.js 和 Edge Impulse CLI：
   ```bash
   npm install -g edge-impulse-cli
   ```

2. 登录并连接设备：
   ```bash
   edge-impulse-data-forwarder
   ```
   按提示输入 API Key、选择项目、设置特征名称。

3. 串口数据格式要求：
   ```
   0.12,0.45,0.78,0.23,0.91,-0.02,0.15,0.98,12.5,-8.3,4.1
   ```
   每行 11 个数值，不能有额外文字。

4. 采集时打标签：
   ```bash
   edge-impulse-data-forwarder --labels good,bad,thanks,eat,water,i,want,doctor,help,cold
   ```

**方式二：CSV 批量上传（离线采集后上传）**

1. 先用 SD 卡或串口保存 CSV 文件：
   ```csv
   timestamp,flex0,flex1,flex2,flex3,flex4,accX,accY,accZ,gyroX,gyroY,gyroZ,label
   0,0.12,0.45,0.78,0.23,0.91,-0.02,0.15,0.98,12.5,-8.3,4.1,good
   10,0.13,0.46,0.77,0.24,0.90,-0.03,0.14,0.97,12.3,-8.1,4.2,good
   ```

2. 在 Edge Impulse Studio 中点击 **Data acquisition** → **Upload existing data**
3. 选择 CSV 文件，设置标签列名为 `label`，时间戳列名为 `timestamp`

---

## 三、Edge Impulse Studio 配置

### 3.1 创建项目

1. 登录 https://studio.edgeimpulse.com
2. 点击 **Create new project**
3. 项目名：`LingxiGlove-Gesture-Recognition`
4. 选择 **Accelerometer data**（虽然我们不是纯加速度计，但这个选项最适合时间序列分类）

### 3.2 配置 Impulse（数据预处理流程）

点击左侧 **Impulse design**，按以下配置：

```
[Input]              → [DSP Block]          → [Learning Block]     → [Output]
Time series data     → Flatten / Spectral   → Classification (Keras) → 10 classes
```

**具体参数设置：**

| 参数 | 值 | 说明 |
|------|-----|------|
| Window size | 2000 ms | 2 秒时间窗口 |
| Window increase | 200 ms | 滑动步长 200ms，产生 90% 重叠 |
| Sampling frequency | 100 Hz | 与采集频率一致 |
| Data axis | 11 | 11 维特征 |
| Labeling method | One label per window | 每个窗口一个标签 |

**DSP Block 选择：**

对于手势识别，推荐选择 **Flatten**（展平）而不是 Spectral Features：

- Flatten：将时间窗口内的所有数据点展平为特征向量，适合 CNN 自己学习时间特征
- Spectral Features：提取频域特征（FFT、峰值等），适合振动分析，但会丢失时序细节

**Flatten 参数：**
- Scale axes：全部设为 `1.0`（已经在 Arduino 端归一化）
- Filter：选择 `Low-pass` 或 `None`（如果数据已平滑）

### 3.3 生成特征

点击 **Save Impulse** → **Generate features**

Edge Impulse 会自动将原始数据切分为多个窗口，并为每个窗口生成特征向量。

**检查特征可视化：**
- 点击 **Feature explorer**
- 如果不同颜色的点（不同手势）明显聚类分离，说明数据质量好
- 如果混在一起，需要检查：传感器安装是否松动、校准是否准确、手势是否标准

---

## 四、模型训练（1D CNN）

### 4.1 网络架构配置

点击 **NN Classifier**，选择 **Switch to Keras (expert mode)**，输入以下架构：

```python
import tensorflow as tf
from tensorflow.keras.models import Sequential
from tensorflow.keras.layers import Dense, Conv1D, Flatten, Dropout, Reshape, GlobalAveragePooling1D

# 输入形状：(时间步, 特征数) = (20, 11)
# Edge Impulse 自动根据窗口大小和采样率计算时间步

model = Sequential()

# 输入层：将 Flatten 的输出 Reshape 回 2D
model.add(Reshape((20, 11), input_shape=(input_length,)))

# Conv1D 层 1
model.add(Conv1D(32, kernel_size=3, activation='relu', padding='same'))

# Conv1D 层 2
model.add(Conv1D(64, kernel_size=3, activation='relu', padding='same'))

# Global Average Pooling（替代 Flatten，减少参数量）
model.add(GlobalAveragePooling1D())

# Dense 层
model.add(Dense(32, activation='relu'))
model.add(Dropout(0.3))

# 输出层
model.add(Dense(classes, activation='softmax'))
```

**在 Edge Impulse 界面中手动配置（如果不用 Expert mode）：**

| 层 | 类型 | 参数 | 激活函数 |
|----|------|------|----------|
| 0 | Reshape | Target shape: (20, 11) | - |
| 1 | Conv1D | Filters: 32, Kernel: 3, Padding: same | ReLU |
| 2 | Conv1D | Filters: 64, Kernel: 3, Padding: same | ReLU |
| 3 | Global Average Pooling 1D | - | - |
| 4 | Dense | Units: 32 | ReLU |
| 5 | Dropout | Rate: 0.3 | - |
| 6 | Dense | Units: 10 (类别数) | Softmax |

### 4.2 训练参数

| 参数 | 值 | 说明 |
|------|-----|------|
| Number of training cycles | 100 |  epochs |
| Learning rate | 0.001 | Adam 优化器默认 |
| Validation set size | 20% | 从训练集中分出 |
| Test set size | 10% | 从不参与训练的数据中分出 |
| Batch size | 32 | 根据数据量调整 |

### 4.3 数据增强（可选）

如果数据量不足，可以在 **Data augmentation** 中开启：

- **Add noise**：向传感器数据添加高斯噪声，增强鲁棒性（幅度 0.01-0.05）
- **Time warping**：轻微拉伸或压缩时间轴（幅度 5%）
- **Magnitude warping**：缩放信号幅度（幅度 5%）

**注意**：不要过度增强，否则可能改变手势的本质特征。

### 4.4 开始训练

点击 **Start training**。

训练完成后，关注以下指标：

- **Accuracy（准确率）**：目标 ≥ 90%
- **Loss（损失值）**：训练 loss 和验证 loss 都趋于平稳，且差距不大（差距大说明过拟合）
- **Confusion Matrix（混淆矩阵）**：检查哪些手势容易被混淆，针对性增加训练样本

**如果准确率不足 90%，尝试以下调优：**

1. **增加数据量**：每个手势从 50 次增加到 80-100 次
2. **调整网络深度**：尝试增加一个 Conv1D(128) 层
3. **调整学习率**：尝试 0.0005 或 0.002
4. **调整 Dropout**：从 0.3 改为 0.5（如果过拟合）或 0.2（如果欠拟合）
5. **检查数据质量**：重新校准传感器，排除松动或漂移的数据

---

## 五、模型验证与测试

### 5.1 模型性能报告

训练完成后，Edge Impulse 会生成 **Model performance** 报告：

- **On-device performance**：预估在目标设备上的推理延迟和内存占用
- 对于 ESP32-S3，目标：
  - 推理延迟 ≤ 50ms
  - RAM 占用 ≤ 8KB
  - Flash 占用 ≤ 50KB

### 5.2 现场测试

点击 **Live classification**，用串口实时测试：

1. 连接 Arduino Nano ESP32-S3
2. 选择正确的串口和波特率（115200）
3. 打出手势，观察实时分类结果
4. 重点关注容易混淆的手势对（如"吃饭"和"水"都涉及手向嘴边移动）

### 5.3 混淆矩阵分析

常见的混淆场景及解决方案：

| 易混淆手势对 | 原因 | 解决方案 |
|-------------|------|----------|
| 吃饭 vs 水 | 都涉及手向嘴边移动 | 增加 MPU6050 的动态轨迹权重，区分拿筷子状和握杯状 |
| 我 vs 医生 | 食指伸直特征相似 | 强化"医生"的手腕转动（陀螺仪 roll 变化）特征 |
| 谢谢 vs 不 | 动态特征容易重叠 | 增加时间窗口长度，捕获更多时序信息 |

---

## 六、模型导出（Arduino 库）

### 6.1 选择部署目标

点击 **Deployment**，选择：

- **Library**：Arduino
- **Optimizations**：选择 **Quantized (Int8)** —— INT8 量化后模型体积减小 75%，推理速度提升 2-4 倍，准确率损失通常 < 2%
- **Build mode**：选择 **Optimized**

### 6.2 下载库文件

点击 **Build**，Edge Impulse 会自动生成 Arduino 库压缩包（.zip）。

解压后，目录结构如下：

```
signlingua-gesture-recognition_inferencing/
├── src/
│   ├── model_parameters.h       # 模型参数（输入大小、类别数等）
│   ├── model_metadata.h         # 模型元数据
│   └── tflite-model/            # TensorFlow Lite 模型文件
│       ├── tflite_learn_*.cpp
│       └── tflite_learn_*.h
├── examples/
│   ├── nano33ble_sense_microphone_continuous/  # 示例代码
│   └── ...
└── library.properties           # Arduino 库描述文件
```

### 6.3 导入 Arduino IDE

1. 打开 Arduino IDE
2. 点击 **项目** → **加载库** → **添加 .ZIP 库**
3. 选择下载的 `.zip` 文件
4. 在 **文件** → **示例** 中找到对应的示例代码

### 6.4 编写推理代码

基于 Edge Impulse 生成的示例，编写你们的推理代码：

```cpp
#include <signlingua_gesture_recognition_inferencing.h>
#include <Wire.h>
#include <MPU6050.h>

// 特征缓冲区
float features[EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE]; // 20 * 11 = 220
int feature_ix = 0;

// 弯曲传感器引脚
const int flexPins[5] = {A0, A1, A2, A3, A6};
float flexMin[5], flexMax[5];

MPU6050 mpu;

void setup() {
  Serial.begin(115200);
  Wire.begin();
  mpu.initialize();
  
  // 校准（同采集代码）
  // ...
  
  Serial.print("Edge Impulse 模型加载成功，类别数：");
  Serial.println(EI_CLASSIFIER_LABEL_COUNT);
}

void loop() {
  // 采集一帧数据
  float frame[11];
  for (int i = 0; i < 5; i++) {
    int raw = analogRead(flexPins[i]);
    frame[i] = constrain((raw - flexMin[i]) / (flexMax[i] - flexMin[i]), 0.0, 1.0);
  }
  
  int16_t ax, ay, az, gx, gy, gz;
  mpu.getMotion6(&ax, &ay, &az, &gx, &gy, &gz);
  frame[5] = ax / 16384.0;
  frame[6] = ay / 16384.0;
  frame[7] = az / 16384.0;
  frame[8] = gx / 131.0;
  frame[9] = gy / 131.0;
  frame[10] = gz / 131.0;
  
  // 填充特征缓冲区
  for (int i = 0; i < 11; i++) {
    features[feature_ix * 11 + i] = frame[i];
  }
  feature_ix++;
  
  // 当缓冲区满时进行推理
  if (feature_ix >= EI_CLASSIFIER_RAW_SAMPLES_PER_FRAME) {
    feature_ix = 0;
    
    // 创建信号结构
    signal_t signal;
    numpy::signal_from_buffer(features, EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE, &signal);
    
    // 运行推理
    ei_impulse_result_t result = { 0 };
    EI_IMPULSE_ERROR err = run_classifier(&signal, &result, false);
    
    if (err != EI_IMPULSE_OK) {
      Serial.print("推理错误：");
      Serial.println(err);
      return;
    }
    
    // 输出结果
    Serial.print("推理结果：");
    for (size_t i = 0; i < EI_CLASSIFIER_LABEL_COUNT; i++) {
      Serial.print(ei_classifier_inferencing_categories[i]);
      Serial.print(": ");
      Serial.print(result.classification[i].value, 4);
      Serial.print("  ");
    }
    Serial.println();
    
    // 找到最高概率的类别
    float max_val = 0;
    int max_ix = 0;
    for (size_t i = 0; i < EI_CLASSIFIER_LABEL_COUNT; i++) {
      if (result.classification[i].value > max_val) {
        max_val = result.classification[i].value;
        max_ix = i;
      }
    }
    
    if (max_val > 0.7) { // 置信度阈值
      Serial.print("识别手势：");
      Serial.println(ei_classifier_inferencing_categories[max_ix]);
    }
  }
  
  delay(100); // 100Hz
}
```

**关键常量说明：**

| 常量 | 含义 | 典型值 |
|------|------|--------|
| `EI_CLASSIFIER_DSP_INPUT_FRAME_SIZE` | 输入特征总数 | 220 (20帧 × 11维) |
| `EI_CLASSIFIER_RAW_SAMPLES_PER_FRAME` | 时间窗口帧数 | 20 |
| `EI_CLASSIFIER_LABEL_COUNT` | 输出类别数 | 10 |
| `EI_CLASSIFIER_INTERVAL_MS` | 采样间隔 | 100ms |

---

## 七、模型优化与部署

### 7.1 INT8 量化验证

在 Deployment 页面选择 **Quantized (Int8)** 后，对比 FP32 和 INT8 模型的性能：

| 指标 | FP32 | INT8 | 差异 |
|------|------|------|------|
| 模型体积 | ~48KB | ~12KB | ↓ 75% |
| 推理延迟 | ~25ms | ~15ms | ↓ 40% |
| 准确率 | 92.3% | 91.1% | ↓ 1.2% |
| RAM 占用 | ~15KB | ~8KB | ↓ 47% |

**建议**：只要 INT8 量化后的准确率损失 < 3%，就优先使用 INT8。

### 7.2 连续识别策略

单次推理容易受噪声影响，建议实现**滑动窗口投票机制**：

```cpp
const int VOTE_WINDOW = 10;   // 最近 10 次识别结果
const int VOTE_THRESHOLD = 7; // 某手势出现 7 次以上才确认
String voteBuffer[VOTE_WINDOW];
int voteIx = 0;

bool confirmGesture(String candidate) {
  voteBuffer[voteIx] = candidate;
  voteIx = (voteIx + 1) % VOTE_WINDOW;
  
  int count = 0;
  for (int i = 0; i < VOTE_WINDOW; i++) {
    if (voteBuffer[i] == candidate) count++;
  }
  return count >= VOTE_THRESHOLD;
}
```

### 7.3 内存优化

如果 ESP32-S3 出现内存不足（Heap 不够）：

1. **减小 Batch 大小**：在 Edge Impulse 训练时减小 batch size
2. **精简网络**：尝试减少 Conv1D 滤波器数量（32→16，64→32）
3. **降低采样率**：从 100Hz 降到 50Hz，时间窗口帧数从 20 降到 10
4. **使用 PSRAM**：ESP32-S3 有 8MB PSRAM，可以在 `sdkconfig` 中启用

---

## 八、常见问题排查

| 问题 | 可能原因 | 解决方案 |
|------|----------|----------|
| 推理结果全为同一类别 | 输入数据未归一化 | 检查 Arduino 端的 min-max 校准 |
| 准确率很高但现场识别差 | 训练数据与现场佩戴方式不同 | 增加不同佩戴者的样本 |
| 模型体积超过 50KB | 网络太复杂或没开 INT8 量化 | 精简网络，确认选择 Quantized |
| 推理延迟超过 50ms | 采样率太高或网络层数太多 | 降低采样率或简化网络 |
| 某些手势始终识别不对 | 特征区分度不够 | 检查混淆矩阵，增加该手势的负样本 |
| Edge Impulse 无法识别串口 | 波特率不对或数据格式错误 | 确认 115200，纯数值 CSV 格式 |

---

## 九、参考资源

- Edge Impulse 官方文档：https://docs.edgeimpulse.com
- Arduino Nano ESP32 指南：https://docs.arduino.cc/hardware/nano-esp32
- ESP32-S3 技术参考手册：https://www.espressif.com/sites/default/files/documentation/esp32-s3_technical_reference_manual_en.pdf
- 项目参考论文：London Metropolitan University, Sign Language Detection using Smart Glove, 2023

---

**下一步行动**

1. 本周完成 Arduino 数据采集代码烧录和传感器校准
2. 用 Edge Impulse Data Forwarder 采集第一个手势（如"好"）10 次测试流程
3. 确认串口数据格式正确后，批量采集全部 10 个手势
4. 6 月 15 日前完成模型训练并达到 ≥ 90% 准确率

如果在配置过程中遇到具体问题（如串口上传失败、模型准确率上不去），可以随时把错误截图或数据发给我，我帮你定位。
