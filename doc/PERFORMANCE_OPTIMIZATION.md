# LingxiGlove 性能优化记录

> 本文记录 MVP 阶段各轮烧录中针对"手势到出声"端到端延迟的优化方案、参数依据与实测效果。
> 所有优化遵循"先量化瓶颈、再最小侵入改动"原则，并保留完整的失败回落链路。

---

## 一、端到端延迟全景

```
用户做出手势
   ↓
[A] 手势识别       ~50ms     （传感器 20Hz 采样 + 动作门控静止判断）
   ↓
[B] LLM 改写      ~500~2000ms （优化后；原 1500~3000ms）
   ↓
[C] TTS 合成 POST ~400~1000ms （优化后；原 500~1500ms）
   ↓
[D] WAV 下载首包  ~200~500ms  （建连 + 首字节到达）
   ↓
[E] I2S 出声      ~0ms 感知   （流式播放，不计入感知延迟）
   ↓
[F] 播放时长      ~1000~3000ms（按句子长度，不可压缩）
```

**优化前：从手势到出声 ≈ 2.5~6 秒**  
**优化后（P0+P1）：从手势到出声 ≈ 1.5~4 秒（约缩短 30~40%）**

---

## 二、已完成优化

### P0 · TTS 模型升级（第二烧，2026-05-01）

**改动文件**：`src/LingxiGlove_Main/config.h`

```cpp
// 改动前
#define QWEN_TTS_MODEL  "qwen-tts"

// 改动后
#define QWEN_TTS_MODEL  "qwen3-tts-flash"
```

**收益**：
- Qwen3-TTS-Flash 相比 `qwen-tts` 合成速度快约 **30%**（阿里官方数据）
- 两者 REST 接口完全相同（同一 endpoint，同一请求/响应格式）
- 改动范围：一行宏定义，零代码风险，零 SRAM/Flash 变化

**适用场景**：合成质量无明显差异，Flash 版对中文口语短句表现与标准版相当，适合 MVP 验证阶段。

---

### P1 · LLM 改写改为 SSE 流式（第二烧，2026-05-01）

**改动文件**：
- `src/LingxiGlove_Main/http_client.h` — 新增 `httpPostJsonSse()` 声明
- `src/LingxiGlove_Main/http_client.cpp` — 实现 SSE 逐行读取
- `src/LingxiGlove_Main/llm_client.cpp` — `chatQwen` 改用 SSE 模式

#### P1.1 问题定位

LLM 改写（`rewriteGestureToSentence`）是整个链路的最大瓶颈：

```
非流式 REST 模式：
  POST → [等待服务端生成全部 token] → 完整 JSON 响应 → 解析
  等待时间：1500~3000ms

SSE 流式模式：
  POST → [服务端逐 token 推送 data: event] → 逐帧解析累积 → 收到 stop 立即返回
  等待时间：500~2000ms（减少约 800~1500ms）
```

虽然"总 token 生成时间"云端不变，但 SSE 的关键优势是：
1. **省去等待"最后一个 token"的尾部延迟**——服务端 `finish_reason=stop` 一到就断连，不需要等完整 HTTP body 传输完
2. **为未来真流水线（LLM 首 token → 立即送 TTS）预留接口**（当前已埋桩：收到每个 delta 就累积，`incremental_output=true`）

#### P1.2 SSE 接口设计

新增 `httpPostJsonSse()`：

```cpp
int httpPostJsonSse(const char* url,
                    const String& jsonPayload,
                    const char* authHeader,
                    std::function<bool(const String& line)> onLine);
```

- 发送 `X-DashScope-SSE: enable` + `Accept: text/event-stream` 请求头
- 用 `HTTPClient::getStreamPtr()` 逐字节读取，以 `\n` 为行分隔符
- 每拼出一行就调用 `onLine` 回调；回调返回 `false` 立即停止读流（提前退出）
- 使用 `static char s_line_buf[512]` 栈上缓冲避免频繁 `String` 扩容

#### P1.3 chatQwen SSE 实现要点

DashScope 文字生成 SSE 帧格式（启用 `incremental_output=true`）：

```
data: {"output":{"choices":[{"message":{"content":"我"},"finish_reason":"null"}]},...}
data: {"output":{"choices":[{"message":{"content":"想"},"finish_reason":"null"}]},...}
data: {"output":{"choices":[{"message":{"content":"吃饭"},"finish_reason":"stop"}]},...}
data: [DONE]
```

- 每帧 content 是**增量 delta**（单字或词组），通过累积拼接还原完整句子
- 遇到 `finish_reason=stop` 或 `[DONE]` 立即调回调返回 `false`，终止读流
- 对外 `chatLLM()` 签名不变 → `rewriteGestureToSentence` 和主循环**零改动**

#### P1.4 内存影响

| 项目 | 变化 |
|------|------|
| Flash | +4.8 KB（SSE 函数 + lambda + std::function 模板实例化） |
| SRAM 静态 | +512 B（`s_line_buf` static 缓冲） |
| SRAM 运行时堆 | +0（`accumulated` 预 reserve(160) 字节，拼接期在 heap，函数返回后释放） |
| 编译后全量 | 947 KB / 3145 KB (30%)，SRAM 62 KB / 327 KB (19%) |

---

## 三、已评估但暂不实施的优化

### WebSocket Realtime TTS（未做，评级 P2）

**结论：MVP 阶段性价比不足，建议第三烧后重新评估。**

DashScope 确实提供 `qwen-tts-realtime` WebSocket API（`wss://` 持久连接 + 事件驱动），可带来：
- 省去二次 HTTP GET 建连开销：~200~500ms
- 首字出声更快：~300~800ms（服务端逐片推送 PCM base64 delta）

但在 ESP32-S3 上实施的代价：
- `wss://` 需要 TLS，ESP32 Arduino 生态里无高质量 wss 客户端库
- 每帧需要 base64 decode → PCM，比当前直接流式读 WAV binary 复杂
- WebSocket 连接管理（心跳/重连/`session.finish`）增加维护成本
- `loopTask` 8 KB 栈在 WS + JSON + base64 + I2S 调用链下有溢出风险

### LLM + TTS 真流水线（未做，评级 P3）

**远期方案**：LLM 输出前几个词就立刻送 TTS 流式合成（边生成边播），端到端延迟可降到 1~2s。

前提条件：
1. P1 的 LLM SSE 流式已就位（已完成，`incremental_output=true` 已开启）
2. TTS 支持增量文本输入（DashScope `qwen-tts-realtime` WebSocket 的 `ServerCommit` 模式）
3. 需要协调 LLM token 速率与 TTS 合成速率，避免 TTS 缓冲区饥饿

当前已在 `chatQwen` 里埋桩（增量模式），实施 P3 时只需扩展回调接口。

---

## 四、烧录阶段调试优化（历史记录）

以下优化在烧录调试过程中发现并实施，与延迟无关但影响稳定性。

### H1 · HTTP 改 HTTPS + 拉长超时（第一烧）

**改动**：`http_client.cpp` 改用 `WiFiClientSecure + setInsecure()`，timeout 从默认 ~5s 拉到 15s。

**原因**：DashScope 端点为 `https://`，原始 `HTTPClient` 走 HTTP 不支持 TLS；超时过短在 -70 dBm 环境下常见 "read Timeout"。

### H2 · 加大 I2S DMA 缓冲（第一烧）

**改动**：`tts_player.cpp` 中 `dma_buf_count 8→16, dma_buf_len 512→1024`（总 DMA 深度 32 KB）。

**原因**：WiFi 抖动 >170ms 时 I2S 会播放静音帧，表现为"咔哒"顿挫；加大 DMA 深度后抗 500ms 级 WiFi 停顿。

### H3 · 下载缓冲改 static 避免栈溢出（第一烧）

**改动**：`speak()` 内 `uint8_t buffer[4096]` 改为 `static uint8_t s_download_buf[4096]`。

**原因**：loopTask 默认 8 KB 栈，WAV 下载缓冲叠加 WiFiClientSecure + mbedtls 调用栈会超限，触发 stack canary panic 导致复位循环。

---

## 五、后续优化 Roadmap

| 优先级 | 方案 | 预期收益 | 前提条件 |
|--------|------|---------|---------|
| **P0 ✅** | TTS 模型 `qwen-tts` → `qwen3-tts-flash` | TTS 合成快 ~30% | 无 |
| **P1 ✅** | LLM 改写改 SSE 流式 | 减少 800~1500ms | P0 |
| **P2** | WebSocket Realtime TTS | 再减 500~1300ms | 需稳定 wss 库，评估第三烧后 |
| **P3** | LLM+TTS 真流水线（首 token 即送 TTS） | 总端到端 <2s | P1 + P2 完成后 |

---

## 六、参考资料

- [DashScope Qwen-TTS REST API 文档](https://help.aliyun.com/zh/model-studio/qwen-tts-api)
- [DashScope Qwen-TTS Realtime WebSocket 文档](https://help.aliyun.com/zh/model-studio/qwen-tts-realtime)
- [DashScope SSE 流式输出说明](https://help.aliyun.com/zh/model-studio/text-generation-api) (`X-DashScope-SSE: enable`)
- [ESP32 I2S DMA 缓冲优化](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/peripherals/i2s.html)
