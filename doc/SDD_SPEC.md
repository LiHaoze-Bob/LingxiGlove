# LingxiGlove SDD 设计规范（SDD_SPEC.md）

> **版本**：v1.3.0  ·  **日期**：2026-05-23  ·  **作者**：Lingxi Team
> **配套文件**：[`spec.yaml`](../spec.yaml)（机器可读契约） / [`DEVELOPMENT.md`](DEVELOPMENT.md)（开发指南） / [`SOLUTION_REVIEW.md`](SOLUTION_REVIEW.md)（方案 Review）

---

## 0. 文档定位与三角分工

本项目的设计文档采用「契约 / 指南 / 评审」三角分工，互不重复：

| 文档 | 定位 | 受众 | 变更频率 |
|------|------|------|----------|
| **`spec.yaml` + `SDD_SPEC.md`（本文）** | **产品契约 / 设计总纲**：硬件接口、模块边界、阶段路线、约束、测试矩阵、降级策略 | 全员对齐共识；新人入职第一份读物 | 低，重大架构变更才改 |
| `DEVELOPMENT.md` | **操作手册**：接线表、烧录步骤、串口命令、调试 FAQ、当前物料盘点 | 开发者动手时查 | 高，每次硬件/命令变化都改 |
| `SOLUTION_REVIEW.md` | **方案评审 / 答辩素材**：技术风险、改进建议、双手方案选型论证、TDOA 仿真结果 | 答辩 + 立项评审 | 阶段性归档 |

> 规则：**"是什么"放本文，"怎么干"放 DEVELOPMENT，"为什么这么干"放 SOLUTION_REVIEW**。

---

## 1. 产品概述

### 1.1 一句话定位
**双节点（左右手）+ 端云协同 + 三模态输出（语音 / 振动预留 / OLED 预留）的低成本手语翻译手套**，面向 **2026 全国大学生物联网设计竞赛 Espressif 赛道**。

### 1.2 价值主张
- **社会价值**：为 2780 万听障群体打通"手语 → 自然语音"的实时沟通通道
- **技术价值**：用 ESP32-S3 + ESP-NOW 把双手时空同步做到链路层级，端侧规则识别 → 1D-CNN → 云端 LLM 形成三级响应
- **成本优势**：单手套 BOM ≈ ¥250（商用 ¥5k–¥80k 量级），两个数量级降本

### 1.3 V0 → V1 → V2 演进
| 阶段 | 端侧 | 云端 | 词表 | 状态 |
|------|------|------|------|------|
| **MVP-v1.1** | RuleBased + BimanualRule | Qwen-LLM 改写 + Qwen-TTS | 单手 5 + 双手 5 | ✅ 当前 |
| **V1.0** | Edge Impulse 1D-CNN（双流） | 同上 | 同上但准确率 ≥90% | 🔄 P3 进行中 |
| **V2.0** | 端侧分级触发 | + Sign-Transformer 扩词表 | 开放词汇 | 🗓️ P5–P6 |

---

## 2. 系统五层架构

```
┌──────────────────────────────────────────────────────────┐
│  L5  应用主控  GestureApp / FingerSpelling / 采集模式     │
│      LingxiGlove_Main.ino  (识别 / 校准 / 串口命令分发)   │
├──────────────────────────────────────────────────────────┤
│  L4  AI 与端云                                            │
│      gesture_recognizer  ←  motion_detector (静止门控)    │
│      llm_client (Qwen/百度) → tts_player → I2S            │
│      local_tts_fallback ← offline_voice_pcm  (断网兜底)   │
├──────────────────────────────────────────────────────────┤
│  L3  双手协同                                             │
│      esp_now_sync (HandFrame, MASTER ↔ SLAVE)             │
│      nvs_config (运行时角色 / 凭证 / 校准持久化)          │
├──────────────────────────────────────────────────────────┤
│  L2  传感器与执行器                                       │
│      sensor_manager (MPU6050 + Flex ADC + 姿态解算)       │
│      calibration (IMU 零偏 + Flex 量程, NVS)              │
│      tts_player (I2S + MAX98357A)                         │
├──────────────────────────────────────────────────────────┤
│  L1  Arduino + FreeRTOS + ESP-IDF (HAL)                   │
└──────────────────────────────────────────────────────────┘
```
> 详图见 [`doc/images/图1_系统五层架构图.png`](images/图1_系统五层架构图.png) 与 [`图2_软件任务调度与数据流图.png`](images/图2_软件任务调度与数据流图.png)。

---

## 3. 硬件契约

### 3.1 双节点拓扑
- **Master（右手）**：MPU6050 + Flex×1（拇指）+ MAX98357A 喇叭，承担识别、LLM、TTS、播放
- **Slave（左手）**：MPU6050（仅）；通过 ESP-NOW 上报 `HandFrame` 给 Master
- 单一固件，**NVS + 串口命令 `role master/slave` 切换角色**，无需重编译

### 3.2 关键引脚约束（不可改）

| 用途 | Master 引脚 | 备注 |
|------|--------|------|
| MPU6050 SDA / SCL | GPIO11 (A4) / GPIO12 (A5) | I2C 占用，**Flex 必须避开 A4/A5** |
| I2S BCLK / LRC / DIN | GPIO7 / 8 / 9 (D4/D5/D6) | MAX98357A |
| Flex 拇指 AO | A0（默认） | P2 启用前 `ENABLE_FLEX_SENSORS=0` |
| 板载蓝色 LED | GPIO45 (D16) | ESP-NOW 配对成功亮 |
| RST 引脚 | **悬空，不插扩展板** | 否则 USB 端口会消失 |

完整接线表见 [`DEVELOPMENT.md §2.2`](DEVELOPMENT.md#22-关键接线说明)。

### 3.3 物料状态
见 [`DEVELOPMENT.md §10.1`](DEVELOPMENT.md#101-当前物料盘点)（动态更新）。本 SPEC 仅锁定**到货且已验证**的部件作为契约。

---

## 4. 软件模块契约

源码扁平结构（Arduino sketch 风格）：[`src/LingxiGlove_Main/`](../src/LingxiGlove_Main)。**15 个核心 cpp 模块** + 1 个 sketch 入口。

| 模块 | 文件 | 公开数据类型 | 关键不变量 |
|------|------|-------------|-----------|
| **sensor_manager** | `sensor_manager.{h,cpp}` | `SensorData{ax,ay,az,gx,gy,gz,pitch,roll,flex[5],flexValid,timestamp_ms}` | 采样周期 50 ms；`ENABLE_FLEX_SENSORS=0` 时 `flexValid=false` |
| **gesture_recognizer** | `gesture_recognizer.{h,cpp}` | `GestureResult{label,confidence,timestamp_ms}`；抽象基类 + RuleBased + BimanualRule | 防抖 500 ms；冷却 2 s |
| **gesture_arbitrator** | `gesture_arbitrator.{h,cpp}` | `GestureCandidate{source,text,confidence}` → `ArbitratedGesture{should_announce,source,text,confidence}` | 确认窗口 200 ms；冷却 2 s；双手优先抑制单手 |
| **motion_detector** | `motion_detector.{h,cpp}` | `MotionState{isMoving,acc_var,gyro_norm}` | 双阈值门控；不可在热路径分配内存 |
| **calibration** | `calibration.{h,cpp}` | `CalibrationData{imu_bias[6],flex_min[5],flex_max[5]}` | NVS key 不变；启动自动加载 |
| **esp_now_sync** | `esp_now_sync.{h,cpp}` | `HandFrame{master_ts,seq,frame_type,proto_version,a/g[6],flex[5]}` 30B | 信道随 AP；`ENABLE_ESPNOW_SYNC` 受控 |
| **nvs_config** | `nvs_config.{h,cpp}` | `RoleConfig{role,wifi_ssid,wifi_pwd,...}` | 运行时改、重启即生效 |
| **wifi_manager** | `wifi_manager.{h,cpp}` | — | 守护任务，断线 30 s 内重连 |
| **http_client** | `http_client.{h,cpp}` | — | 内置 URL encode；超时可配 |
| **llm_client** | `llm_client.{h,cpp}` | `LlmResponse{text,ok,err}` | Qwen / Baidu 双 provider；token 过期回退 |
| **tts_player** | `tts_player.{h,cpp}` | `Speak(text)` / `PlayPcmInt16(buf,len,sr)` | 总超时 15 s；空读超时 3 s；LittleFS 缓存 |
| **local_tts_fallback** | `local_tts_fallback.{h,cpp}` | — | TTS 失败时按 label 命中离线 PCM |
| **offline_voice_pcm** | `offline_voice_pcm.{h,cpp}` | `PcmEntry[]` 表 | 由 `tools/gen_offline_voice_pcm.py` 生成；空表降级蜂鸣 |
| **accuracy_test** | `accuracy_test.{h,cpp}` | 混淆矩阵 + LittleFS `/acc_test/` CSV 日志 + `test` 串口命令族 | 现场答辩用；不触发 TTS/LLM |
| **secrets** | `secrets.{example.h,h}` | `WIFI_SSID/QWEN_API_KEY/BAIDU_*` | `secrets.h` 永不入库 |

### 4.1 `GestureRecognizer` 抽象（核心扩展点）

```cpp
class GestureRecognizer {
public:
    virtual ~GestureRecognizer() = default;
    virtual bool init() = 0;
    virtual GestureResult recognize(const SensorData& data) = 0;
    virtual const char* getName() const = 0;
};
```
- **MVP**：`RuleBasedRecognizer`（俯仰/横滚 + 拇指弯曲），`BimanualRuleRecognizer`（双手协同规则）
- **V1**：`EdgeImpulseRecognizer`（导出 Arduino 库后接入），保持同一接口、零改动主循环

### 4.2 `GestureArbitrator`（统一决策层）

```cpp
struct GestureCandidate {
    GestureSource source;     // SINGLE_HAND / BIMANUAL
    const char*   text;
    float         confidence;
};

class GestureArbitrator {
public:
    ArbitratedGesture tick(const GestureCandidate& single,
                           const GestureCandidate& bimanual,
                           unsigned long now_ms);
};
```
- **模型无关**：任何识别器（规则/CNN）只需输出 `GestureCandidate` 即可接入
- **核心规则**：双手候选存在时抑制单手（解决"不" vs "帮助"竞态）；确认窗口 200 ms；冷却 2 s；冷却后 pending 强制重置

---

## 5. AI Pipeline 数据流

```
20Hz 双手 IMU + Flex
        │  (Slave: ESP-NOW → Master)
        ▼
sensor_manager.read()  →  motion_detector.gate()
        │  movement?
        ▼
GestureRecognizer.recognize()   ← 防抖 500ms
        │  GestureResult{label, conf}
        ▼
gesture_arbitrator.tick()       ← 确认窗口 200ms + 冷却 2s
        │  ArbitratedGesture{should_announce, text, conf}
        │  (双手优先抑制单手)
        ▼
      should_announce?
   ┌─────┴────────────┐
   ▼                  ▼
 cloud_path       local_path
   │                  │
   ▼                  ▼
 llm_client        local_tts_fallback
   │  (改写为自然句)   │  (离线 PCM 命中？)
   ▼                  ▼
 tts_player        tts_player.PlayPcmInt16
   │                  │
   └────────┬─────────┘
            ▼
       I2S → MAX98357A → 喇叭
```

降级链路与触发条件见 [`spec.yaml §graceful_degradation`](../spec.yaml)。

---

## 6. 公共契约约定

- **C++11**，禁用异常（`-fno-exceptions`），禁用 RTTI 友好
- **热路径无动态内存分配**：识别 / TTS 推流回调内严禁 `new`/`malloc`
- **所有可变全局状态用 `volatile` 或 FreeRTOS 队列保护**
- **NVS Key 命名空间**：`lx_cfg`（角色 / Wi-Fi）、`lx_cal`（校准）—— 不可重复
- **串口波特率**：固定 `115200`
- **日志宏**：`DEBUG_PRINT/DEBUG_PRINTLN`（可变参数），生产构建可整体关闭

---

## 7. 阶段路线（与 [`spec.yaml §phases`](../spec.yaml) 同步）

| 阶段 | 名称 | 状态 | 验收 |
|------|------|------|------|
| **P0** | 单手 MVP | ✅ 完成 | 规则 + Qwen-TTS 闭环 |
| **P1** | Slave 固件 + ESP-NOW | ✅ 2026-05-14 | "Slave 已连接" + 双板 LED 蓝 |
| **P2** | 弯曲传感器接入 | 🔥 进行中 | flex 列稳定，伸/弯 ADC 差 > 200 |
| **P3** | Edge Impulse 1D-CNN | ⏳ | 10 类 F1 ≥ 0.85，推理 < 50 ms |
| **P4** | 声学 TDOA 测距 | ⏸️ 暂缓 | Python 仿真已完成（见 SOLUTION_REVIEW §2.3） |
| **P5** | 云端 Sign-Transformer | ⏳ | 词表 / Top-k 立项时定 |
| **P6** | 端云分级 + 降级联调 | ⏳ | 断网仍能播核心词 |
| **P7** | 联调 + 答辩 | ⏳ | 演示视频 + 现场对话 |

里程碑追踪：`spec.yaml §milestones` / [`DEVELOPMENT.md §10.3`](DEVELOPMENT.md#103-里程碑进度追踪)。

---

## 8. 测试策略

### 8.1 双层测试金字塔

```
        ▲
        │   现场准确率统计 (accuracy_test 模块)
        │   ───────────────────
        │   Hardware-in-the-Loop
        │   test_speaker / test_acoustic_tdoa
        │   ────────────────────────────────
        │   Host Unit Tests (无硬件依赖, make 即跑, 8 个套件)
        │   test_motion_detector    16 ✅
        │   test_calibration_core   24 ✅
        │   test_local_tts_fallback 20 ✅
        │   test_esp_now_sync       25 ✅
        │   test_bimanual_recognizer 16 ✅
        │   test_arbitrator          ✅
        │   test_llm_rewrite          ✅
        │   test_tts_parsers          ✅
        ▼
```

### 8.2 Host 单测约束
- 不依赖 Arduino 真实库；每个测试目录自带轻量 mock（如 [`test_bimanual_recognizer/Arduino.h`](../src/tests/test_bimanual_recognizer/Arduino.h)）
- 编译选项 `-Werror -Wpedantic -std=c++11`
- 通过 `make` 直接跑，CI 友好

### 8.3 上板测试
- `test_speaker`：I2S 链路自检（PCM 生日歌/语音）
- `test_acoustic_tdoa`：P4 真机 POC（暂缓）
- 其余靠 `accuracy_test` 模块在主程序中开混淆矩阵收集

---

## 9. 性能与资源预算

| 模块 | RAM | Flash | CPU/帧 |
|------|-----|-------|--------|
| sensor_manager | ~256 B | ~3 KB | < 2 ms |
| gesture_recognizer (rule) | ~512 B | ~4 KB | < 1 ms |
| motion_detector | ~128 B | ~1 KB | < 1 ms |
| esp_now_sync | ~1 KB（环形缓冲） | ~3 KB | < 2 ms |
| tts_player + LittleFS 缓存 | ~64 KB（PSRAM） | ~20 KB | I2S DMA 流式 |
| **MVP 合计** | **< 80 KB / 320 KB** | **< 200 KB / 8 MB** | **< 10 ms / 50 ms** |

预留：V1 加入 Edge Impulse 1D-CNN 模型（参数量训练后定，估算 64–128 KB Flash + 32 KB RAM），仍在余量内。详见 [`PERFORMANCE_OPTIMIZATION.md`](PERFORMANCE_OPTIMIZATION.md)。

---

## 10. 已知限制与风险

| 风险 | 影响 | 缓解 | 详细论证 |
|------|------|------|----------|
| 10 词表达力不足 | 无法表达开放语句 | FingerSpelling 兜底（P3+） | SOLUTION_REVIEW §1.2 风险 1 |
| 动态手势分割 | 连续打字粘连 | ✅ motion_detector 静止门控（已实现） | SOLUTION_REVIEW §1.2 风险 2 |
| 跨用户识别率飘 | 不同人识别率差异 | calibration 个体校准 + 多人采集 | SOLUTION_REVIEW §1.2 风险 3 |
| 断网哑火 | 云端不可达即失能 | ✅ local_tts_fallback + offline_voice_pcm（已实现，PCM 表需填充） | SOLUTION_REVIEW §1.2 风险 4 |
| 弯曲传感器单路 | 无法区分细分指型 | 仅承诺握拳 vs 张开两态 | DEVELOPMENT §9 |
| ESP-NOW 信道飘 | 双手丢帧 | 强制 STA 同 AP 同步信道 | DEVELOPMENT §10.2 P1 |

---

## 11. 变更记录

| 版本 | 日期 | 变更 | 作者 |
|------|------|------|------|
| v1.3.0 | 2026-05-23 | §4 HandFrame 28B→30B（补充 frame_type/proto_version）；§4 accuracy_test 补充 LittleFS CSV；§8 测试套件补充 test_arbitrator，总数 8 个；§10 动态分割/断网哑火标记已缓解 | Lingxi Team |
| v1.2.0 | 2026-05-22 | §4 新增 gesture_arbitrator 模块（15 模块）；§4.2 统一决策层接口；§5 数据流加入仲裁层 | Lingxi Team |
| v1.1.0 | 2026-05-22 | 重写以对齐双节点架构、14 模块、P0–P7 实际进度；废弃旧版 `mpu6050.cpp/flex_sensor.cpp/gesture_classifier.cpp` 假设结构 | Lingxi Team |
| v0.1.0 | 2026-05-22 | 初版（已废弃） | — |

---

## 12. 如何使用本规范

- **新人入职**：本文 + `README.md` + `DEVELOPMENT.md §1–7` 看完即可上手
- **提 PR 前**：检查改动是否破坏 §4 模块契约 / §6 公共约定 / §9 资源预算；必要时同步 `spec.yaml`
- **加新模块**：在 `spec.yaml §software.modules` 与本文 §4 表格各加一行；变更类型时同步 §5 数据流图
- **阶段推进**：`spec.yaml §phases` 与 §milestones 同步；本文 §7 表格同步状态
- **性能断言改动**：必须在 `PERFORMANCE_OPTIMIZATION.md` 或 `SOLUTION_REVIEW.md` 留下实测证据，**不允许凭估算改 §9 数字**
