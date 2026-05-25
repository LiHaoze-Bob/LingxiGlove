# 灵犀手套 Lingxi — 使用指南

> Arduino Nano ESP32-S3 智能手语翻译手套的烧录、配置与串口交互指南。

---

## 目录

- [硬件准备](#硬件准备)
- [Arduino IDE 配置](#arduino-ide-配置)
- [首次烧录](#首次烧录)
- [串口命令参考](#串口命令参考)
- [运行模式说明](#运行模式说明)
- [WiFi 配置](#wifi-配置)
- [ESP-NOW 双板配置](#esp-now-双板配置)
- [TTS 语音播报](#tts-语音播报)
- [故障排查](#故障排查)

---

## 硬件准备

| 物料 | 型号 | 数量 | 用途 |
|------|------|:----:|------|
| 主控板 | Arduino Nano ESP32-S3 (ABX00083) | 2 | 主手 MASTER + 从手 SLAVE |
| IMU | MPU6050 | 2 | 六轴姿态感知 |
| I2S 功放 | MAX98357A | 1 | 语音输出（仅 MASTER） |
| 弯曲传感器 | 单路模块 | 1~5 | 手指弯曲检测 |
| 麦克风 | INMP441（可选） | 2 | 声学测距（P4 阶段） |

### 接线参考

**MPU6050（I2C）**：
- SDA → A4 / GPIO11
- SCL → A5 / GPIO12

**MAX98357A（I2S）**：
- BCLK → D4 / GPIO7
- LRC → D5 / GPIO8
- DIN → D6 / GPIO9

---

## Arduino IDE 配置

### Board 选择

- **Board**: Arduino Nano ESP32

### Tools 菜单关键设置

| 选项 | 必须选择 | 说明 |
|------|---------|------|
| **PSRAM** | OPI PSRAM | TTS 音频缓冲 192KB 放 PSRAM，不开会 DRAM 溢出 |
| **Partition Scheme** | With SPIFFS partition (advanced) | LittleFS TTS 缓存需要 SPIFFS 分区（9.375MB） |
| **USB CDC On Boot** | Enabled | 串口调试需要 |

### 密钥配置

```bash
cd src/LingxiGlove_Main
cp secrets.example.h secrets.h
```

编辑 `secrets.h` 填入：

```c
#define WIFI_SSID       "你的WiFi名称"
#define WIFI_PASSWORD   "你的WiFi密码"
#define QWEN_API_KEY    "sk-xxxxxxxxxxxxxxxx"
```

> ⚠️ `secrets.h` 已被 `.gitignore` 忽略，禁止提交到仓库。

### 角色切换（编译期）

sketch 目录下有预置模板文件：

```bash
# 烧录为 MASTER（右手）
cp build_opt.h.master build_opt.h

# 烧录为 SLAVE（左手）
cp build_opt.h.slave build_opt.h
```

> 也可通过串口运行时切换角色（见下文），无需重新编译。

---

## 首次烧录

1. 选择正确的 Board / Port / PSRAM / Partition Scheme
2. 确认 `secrets.h` 已填写
3. 点击 Upload
4. 打开 Serial Monitor（波特率 **115200**）
5. 观察启动日志，确认 WiFi 连接成功、传感器初始化正常

---

## 串口命令参考

波特率：**115200**，行尾：CR+LF 或 LF 均可。

### 模式切换

| 命令 | 功能 |
|------|------|
| `r` | 恢复**识别模式**（正常手势识别 + TTS 播报） |
| `c` | 进入**词级数据采集模式**（CSV 输出，用于 Edge Impulse 训练） |
| `f` | 进入**指拼采集模式**（CSV 输出，为未来指拼字母表模型预留） |

### 校准与调试

| 命令 | 功能 |
|------|------|
| `k` | 执行**个体零偏校准**（~3 秒，手套平放静止） |
| `i` 或 `info` | 打印当前设备信息（角色、MAC、WiFi SSID） |
| `h` 或 `?` | 显示帮助 |

### TTS 与 LLM

| 命令 | 功能 |
|------|------|
| `t` | **手动 TTS 播报**：输入 `t` 后在 5 秒内输入文本并回车 |
| `l` | **LLM 改写 + TTS**：输入 `l` 后在 5 秒内输入手势序列并回车 |

示例：
```
t 你好世界
l 我,吃饭
```

### WiFi 配置（运行时）

| 命令 | 功能 |
|------|------|
| `wifi <SSID> <PASSWORD>` | 设置 WiFi 凭据，写入 NVS 并重启 |
| `wifi <SSID>` | 设置无密码的开放网络 |
| `wifi clear` | 清除 NVS WiFi 配置，恢复 `secrets.h` 默认值并重启 |

示例：
```
wifi MyNetwork MyPassword123
wifi clear
```

> **优先级**：NVS 已保存值 > `secrets.h` 编译期宏。清除后恢复到 `secrets.h` 中的默认值。

### ESP-NOW 角色与配对（运行时）

| 命令 | 功能 |
|------|------|
| `role master` | 设为 MASTER 角色，写入 NVS 并重启 |
| `role slave` | 设为 SLAVE 角色，写入 NVS 并重启 |
| `peer AA:BB:CC:DD:EE:FF` | 设置对端 MAC 地址，写入 NVS 并重启 |
| `nvs clear` | 清除所有 NVS 配置（角色 + MAC），恢复编译期默认值并重启 |

> **优先级**：NVS > `build_opt.h` 编译期宏 > `config.h` 默认值。

### 准确率测试

| 命令 | 功能 |
|------|------|
| `test <id> <count>` | 启动准确率测试会话（如 `test 1 30` = 测试"你好"×30 次） |
| `test cancel` | 取消当前测试会话 |
| `test export` | 将所有历史测试 CSV 日志 dump 到串口 |
| `test clear` | 清除所有测试日志 + 重置计数器 |
| `test help` | 打印手势 ID 对照表和使用说明 |

手势 ID 对照：
- **单手**：1=你好, 2=谢谢, 3=再见, 4=是, 5=不
- **双手**：101=加油, 102=一起, 103=我爱你, 104=帮助

> 准确率测试模式下**不触发 TTS/LLM**，仅评测识别器命中率，结果写入 LittleFS `/acc_test/` 目录。

### 典型双板配对流程

```bash
# 1. 板 A 串口，查看 MAC
info
#    输出: 本机 MAC: AA:BB:CC:DD:EE:01

# 2. 板 B 串口，查看 MAC
info
#    输出: 本机 MAC: AA:BB:CC:DD:EE:02

# 3. 板 A 设为 MASTER，指向板 B
role master
peer AA:BB:CC:DD:EE:02

# 4. 板 B 设为 SLAVE，指向板 A
role slave
peer AA:BB:CC:DD:EE:01

# 配对成功后 LED 常亮蓝色
```

---

## 运行模式说明

### 识别模式（默认）

正常工作模式。传感器 20Hz 采样 → 动作门控 → 手势识别 → LLM 改写 → TTS 播报。

- 单手手势直接由本机 MASTER 识别
- 双手手势需 SLAVE 通过 ESP-NOW 发送 HandFrame，MASTER 融合后识别

### 采集模式

按 `c` 进入。传感器数据以 CSV 格式输出到串口，用于离线训练（Edge Impulse）。

CSV 列：`timestamp, ax, ay, az, gx, gy, gz [, flex0..flex4]`

按 `r` 返回识别模式。

---

## WiFi 配置

系统启动时会自动连接 WiFi。WiFi 凭据加载优先级：

1. **NVS 存储**（通过 `wifi` 命令设置，掉电不丢失）
2. **`secrets.h` 编译期宏**（NVS 无记录时的兜底）

### 离线模式

已缓存的 TTS 词汇可在无 WiFi 时播放。首次合成需联网，后续直接从 Flash 读取（<100ms）。

---

## TTS 语音播报

### 播放链路

```
手势识别 → LLM 改写（可选）→ LittleFS 缓存查询
  ├─ 命中 → 从 Flash 读取 WAV → I2S 播放（<100ms）
  └─ 未命中 → 云端 Qwen-TTS 合成 → 下载 WAV → 写缓存 → I2S 播放（2-4s）
```

### 音量调节

编辑 `config.h`：

```c
#define TTS_VOLUME_GAIN  2.0f   // 范围 1.0~4.0，2.0 = +6dB
```

硬件调节（更推荐）：MAX98357A GAIN 引脚接 100KΩ 到 GND → 15dB。

---

## 故障排查

| 现象 | 可能原因 | 解决方法 |
|------|---------|---------|
| 编译报 `dram0_0_seg overflowed` | PSRAM 未开启 | Tools → PSRAM → OPI PSRAM |
| `LittleFS 初始化失败` | 分区表错误 | Tools → Partition Scheme → With SPIFFS partition |
| WiFi 连接失败循环重试 | SSID/密码错误 | `wifi <正确SSID> <正确密码>` 或修改 `secrets.h` |
| ESP-NOW 收不到帧 | 两板不在同一 WiFi 信道 | SLAVE 也需连同一 AP（自动同步信道） |
| TTS 无声音 | I2S 接线错误 | 检查 BCLK/LRC/DIN 是否对应 GPIO7/8/9 |
| 串口无输出 | USB CDC 未开启 | Tools → USB CDC On Boot → Enabled |
| `secrets.h` 编译找不到 | 未复制模板 | `cp secrets.example.h secrets.h` |

---

## 演示词汇（10 句）

| 类型 | 词汇 |
|------|------|
| 单手 | 你好、谢谢、再见、明白了、不 |
| 双手 | 加油、一起、帮助、我爱你、我们走 ⚠️ |

> ⚠️ "我们走"为规划词汇，代码中尚未实现（BimanualRuleRecognizer 当前支持 4 个双手手势）。

---

*最后更新：2026-05-23*
