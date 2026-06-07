# LingxiCapture 技术架构（Day 5 版）

> 本文档对照仓库当前实现整理而成（Day 1 ~ Day 5 全部交付完成 + 后续小修）。
> 所有引用均指向源文件实际代码，可作为新成员上手 / Code Review / 重构基线。

---

## 0. TL;DR — 一张图看懂

```
端侧 firmware (LingxiGlove_Main.ino)
   ├── MASTER 板         ── ESP-NOW ──►   SLAVE 板
   │   USB Serial CSV(29 列)                      │
   │      │                                       │
   ▼      ▼                                       ▼
┌───────────────────────── Tauri Process ─────────────────────────────┐
│                                                                     │
│  Tokio Runtime (Rust)                                               │
│  ┌───────────────────────────────────────────────────────────────┐  │
│  │  SerialTask("left")    [仅 1 个！UI 上叫 "Master"]            │  │
│  │   ├── tokio_serial AsyncRead/Write 拆 read/write half          │  │
│  │   ├── LinesCodec 按行解析                                      │  │
│  │   ├── 200ms 主动 'i' 探针 + 5s detect_role 兜底 → emit role   │  │
│  │   ├── parse_line: 14 → 单手 1 帧 / 28 → 拆 left+right 2 帧    │  │
│  │   └── mpsc::Sender<Frame>                                      │  │
│  │                            │                                   │  │
│  │                            ▼                                   │  │
│  │  Aggregator                                                    │  │
│  │   ├── 注入 LabelState.get() （PC 端权威）                      │  │
│  │   ├── FpsCounter.tick()                                        │  │
│  │   ├── SessionState.on_frame_obj():                             │  │
│  │   │     bimanual_raw=Some → bimanual writer (29 列)            │  │
│  │   │     raw_line 非空      → alias writer (15 列)              │  │
│  │   └── app.emit("frame", ...)                                   │  │
│  └───────────────────────────────────────────────────────────────┘  │
│                                  ▼ Tauri IPC                        │
│  WebView (React + Vite + zustand)                                   │
│  ┌───────────────────────────────────────────────────────────────┐  │
│  │  listen("frame")          → uPlot ring buffer (rAF setData)   │  │
│  │  listen("serial-role")    → store.masterRole / captureFlow    │  │
│  │  listen("pipeline-progress") → PipelinePanel 日志/进度        │  │
│  │                                                                │  │
│  │  状态机 captureFlow:                                           │  │
│  │  DISCONNECTED → HANDSHAKING → IDLE                             │  │
│  │       ↑                       │ Space                          │  │
│  │       │                       ▼                                │  │
│  │  FINISHING ←  RECORDING  ← COUNTDOWN                           │  │
│  │   (stop_session)  (sendChar 'c')                               │  │
│  └───────────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────────┘
                                  │
                                  ▼ 子进程 / HTTP
                  python build_dataset.py  →  Edge Impulse Ingestion
```

**核心三个铁律**：

1. **CSV 兼容**：raw.csv 仍是端侧 firmware `printCsvHeader()` 字节级一致的 15 / 29 列，
   下游 [`tools/build_dataset.py`](LingxiGlove/tools/build_dataset.py) 0 修改可消费。
2. **时钟统一**：以 PC `SystemTime::now()` 的 `recv_ts_ms` 为全局基准，设备 `millis()` 仅作诊断。
3. **打标统一**：UI 一次 keydown → `set_label` → LabelState（Mutex<i8>） → Aggregator 在每帧上注入。
   端侧 label 列被 PC 端覆写，零分歧。

---

## 1. 进程与运行时拓扑

### 1.1 Tauri 双层

| 层 | 运行时 | 职责 |
|---|---|---|
| Rust 后端 | Tokio 多线程 runtime | 串口 IO / 子进程 / Keychain / HTTP |
| WebView 前端 | Chromium WebKit + V8 | uPlot 实时绘图 / zustand 状态 / 用户交互 |
| 桥接 | Tauri IPC | `invoke(command, args)` + `emit/listen(event)` |

入口：[lib.rs](LingxiGlove_Capture/src-tauri/src/lib.rs#L29-L102)
`run()` 在 setup 钩子里建好 `mpsc::channel(1024)`、`AppState`、`spawn_aggregator(...)`，
随后注册 17 条 command 到 `invoke_handler`。

### 1.2 Day 5 单串口拓扑（与 Day 1 双串口设计的差异）

Day 1 ~ Day 4 假设左右手板各自挂一条 USB；Day 5 改为：

- **UI 上仅暴露 1 条 ConnectBar**，alias 固定 `"left"`（Master）
- Slave 板**通过 ESP-NOW 把数据回灌到 Master**，由 Master 的 capture 模式合成 29 列 CSV 一次性吐出
- 后端在 [`parse_line`](LingxiGlove_Capture/src-tauri/src/serial_task.rs#L179-L249) 里**拆成 left + right 两个 Frame**（兼容前端双 plot 渲染逻辑）

这样做的代价 / 收益：

| 维度 | 双串口（Day 1 设计） | 单 Master（Day 5 实施） |
|---|---|---|
| 用户操作 | 选 2 个 cu.usbmodem | 选 1 个就够 |
| 时钟对齐 | recv_ts 仍能 < 50ms，但 USB 抖动叠加 | 端侧 ESP-NOW 已 lock-step，PC 见到的是同一行 → 0 ms |
| 拆帧逻辑 | 不需要 | 需要 `bimanual_raw` 字段 + lazy bimanual writer |
| 故障隔离 | 一边断不影响另一边 | Slave 离线表现为 slave_age_ms = -1，UI 红色徽章 |

---

## 2. 数据结构（跨边界）

### 2.1 [`Frame`](LingxiGlove_Capture/src-tauri/src/types.rs#L30-L40)

```rust
pub struct Frame {
    pub dev_alias:   DeviceAlias,    // "left" / "right"
    pub recv_ts_ms:  u64,            // PC 全局时间基准
    pub dev_ts_ms:   u32,            // 设备 millis 仅诊断
    pub label:       i8,             // -1 = unlabeled，由 LabelState 注入
    pub values:      Vec<f32>,       // 单手 14 / 拆出的双手 13
    pub raw_line:    String,         // 原始 CSV 行（写盘复用）
    #[serde(default)]
    pub bimanual_raw: Option<String>, // Some(29 列原行) → 进 bimanual writer
}
```

设计要点：
- `bimanual_raw` 是「**路由提示位**」，在 [`SessionState::on_frame_obj`](LingxiGlove_Capture/src-tauri/src/session.rs#L206-L222) 里决定写到哪个 writer
- 拆出的右帧 `raw_line=""` + `bimanual_raw=None`，落盘静默丢弃，但仍 emit 给前端供右手绘图
- `recv_ts_ms` 选 u64 ms：跨边界 serde 友好，前端 `Date.now()` 同源；时差只关心相对值

### 2.2 端侧 CSV schema（不可改）

```
单手 15 列：timestamp_ms,ax,ay,az,gx,gy,gz,pitch,roll,flex0..4,label
双手 29 列：timestamp_ms,m_(13 列),s_(13 列),slave_age_ms,label
```

定义在 [`session.rs::CSV_HEADER` / `CSV_BIMANUAL_HEADER`](LingxiGlove_Capture/src-tauri/src/session.rs#L31-L40)，
**字节级 = 端侧 firmware 的 `printCsvHeader()` / `printBimanualCsvHeader()`**。

---

## 3. 模块清单

### 3.1 Rust 后端 ([src-tauri/src/](LingxiGlove_Capture/src-tauri/src/))

| 文件 | 行数 | 职责 |
|---|---|---|
| [`types.rs`](LingxiGlove_Capture/src-tauri/src/types.rs) | 82 | Frame / DeviceMeta / SerialPortInfo / now_ms |
| [`serial_task.rs`](LingxiGlove_Capture/src-tauri/src/serial_task.rs) | 313 | 单设备 async 串口循环 + parse_line + role 检测 |
| [`aggregator.rs`](LingxiGlove_Capture/src-tauri/src/aggregator.rs) | 123 | mpsc 汇聚 + label 注入 + 写盘路由 + emit |
| [`label.rs`](LingxiGlove_Capture/src-tauri/src/label.rs) | 54 | LabelState（Mutex<i8>） |
| [`session.rs`](LingxiGlove_Capture/src-tauri/src/session.rs) | 427 | SessionWriter / SessionState + bimanual lazy writer + summary |
| [`commands.rs`](LingxiGlove_Capture/src-tauri/src/commands.rs) | 377 | 15 条 Tauri command + AppState |
| [`pipeline.rs`](LingxiGlove_Capture/src-tauri/src/pipeline.rs) | 519 | build_dataset 子进程 + EI 上传 + 一键流水线 |
| [`secrets.rs`](LingxiGlove_Capture/src-tauri/src/secrets.rs) | 50 | macOS Keychain 包装（EI API Key） |
| [`lib.rs`](LingxiGlove_Capture/src-tauri/src/lib.rs) | 104 | 入口：装配所有模块、注册 command |

### 3.2 React 前端 ([src/](LingxiGlove_Capture/src/))

| 文件 | 职责 |
|---|---|
| [`types.ts`](LingxiGlove_Capture/src/types.ts) | 与 Rust types 1:1 镜像 + label 默认表 + channel 名 |
| [`api.ts`](LingxiGlove_Capture/src/api.ts) | invoke 封装 |
| [`store.ts`](LingxiGlove_Capture/src/store.ts) | zustand：currentLabel / sessionInfo / **captureFlow 状态机** / labelNames |
| [`hooks/useGlobalKeydown.ts`](LingxiGlove_Capture/src/hooks/useGlobalKeydown.ts) | 全局快捷键（按状态机分派） |
| [`components/ConnectBar.tsx`](LingxiGlove_Capture/src/components/ConnectBar.tsx) | 串口下拉 + 波特率 + 连/断 |
| [`components/SessionToolbar.tsx`](LingxiGlove_Capture/src/components/SessionToolbar.tsx) | 录制 toolbar（按 captureFlow 渲染） |
| [`components/CountdownOverlay.tsx`](LingxiGlove_Capture/src/components/CountdownOverlay.tsx) | 240px 倒计时数字 + Web Audio beep |
| [`components/RecordingHUD.tsx`](LingxiGlove_Capture/src/components/RecordingHUD.tsx) | 录制中红边脉冲 + 计时 + slave_age 徽章 |
| [`components/LabelHUD.tsx`](LingxiGlove_Capture/src/components/LabelHUD.tsx) | 当前 label 大字号 + 0-9 按钮 + 双击改名 |
| [`components/LabelEditor.tsx`](LingxiGlove_Capture/src/components/LabelEditor.tsx) | 模态批量改名 + 重置默认 |
| [`components/RealtimePlot.tsx`](LingxiGlove_Capture/src/components/RealtimePlot.tsx) | uPlot 单设备多通道折线（ring buffer + rAF） |
| [`components/StatsCard.tsx`](LingxiGlove_Capture/src/components/StatsCard.tsx) | per-device 帧率 / 累计帧 / last raw |
| [`components/PipelinePanel.tsx`](LingxiGlove_Capture/src/components/PipelinePanel.tsx) | session 列表 + 切窗 + EI 上传 + 一键 |
| [`components/SummaryToast.tsx`](LingxiGlove_Capture/src/components/SummaryToast.tsx) | 会话结束摘要弹窗 |
| [`components/OnboardingWizard.tsx`](LingxiGlove_Capture/src/components/OnboardingWizard.tsx) | 首启 5 步引导 |
| [`components/KeyHelp.tsx`](LingxiGlove_Capture/src/components/KeyHelp.tsx) | `?` 呼出快捷键参考 |
| [`components/AboutModal.tsx`](LingxiGlove_Capture/src/components/AboutModal.tsx) | 关于 + 版本 |

---

## 4. 关键技术专题

### 4.1 串口异步 IO + 双工读写（[serial_task.rs](LingxiGlove_Capture/src-tauri/src/serial_task.rs)）

**设计目标**：在不阻塞 Tokio runtime 的前提下，单条串口同时支持「读 ASCII 行」和「写控制字节（c/r/i）」。

**关键代码（精简）**：

```rust
let port = tokio_serial::new(&self.port, self.baud)
    .timeout(Duration::from_millis(10))
    .open_native_async()?;

let (read_half, mut write_half) = tokio::io::split(port);
let mut reader = FramedRead::new(read_half, LinesCodec::new_with_max_length(8192));

loop {
    tokio::select! {
        _ = self.cancel.changed() => return Ok(()),    // disconnect 信号
        _ = &mut probe_timer, if !probe_sent => {       // 200ms 后写 'i' 触发 banner
            write_half.write_all(b"i\n").await?;
        }
        _ = &mut role_timeout, if !role_resolved => {   // 5s 兜底 emit unknown
            app.emit("serial-role", SerialRoleEvent { role: "unknown", .. });
        }
        Some(bytes) = self.write_rx.recv() => {         // 前端 sendChar
            write_half.write_all(&bytes).await?;
        }
        line = reader.next() => { /* parse_line + tx.send */ }
    }
}
```

**为什么不直接用同步 `serialport` crate？**
- macOS / Linux 下 sync read 阻塞整线程；多串口要为每路 spawn `task::spawn_blocking` + 自己拼包
- LinesCodec 自动处理 `\r\n` / `\n` / 半行碎包
- 风险预案：若 tokio-serial mio 触发兼容性问题，回退 sync `serialport` + `spawn_blocking`（PLAN.md §5）

**LinesCodec max_length=8192**：兼容 29 列双手 CSV（约 250 字节）+ 任何长 banner。
**timeout=10ms**：tokio-serial 内部读超时设小一点，让 `select!` 的其他分支（cancel / probe）能及时被调度。

### 4.2 角色握手（200ms 主动探针 + 5s 兜底）

端侧 firmware 在收到 `'i'` 命令时打印 `[配置] 当前角色: MASTER` 这类横幅。

后端做了三件事：
1. **打开串口 200ms 后主动发 `i\n`**（一次性 `probe_sent` flag）
   — 即便板子已 boot 完不再自打 banner，也能拿到角色信号
2. **每行尝试 `detect_role`**：含 `[配置]` 且匹配 `MASTER` / `SLAVE` 关键字 → emit `serial-role` 事件
3. **5s 仍未匹配 → 兜底 emit `unknown`**，前端显示橘色 toast「未识别到角色」

前端 [`store.setMasterRole`](LingxiGlove_Capture/src/store.ts#L211-L212) 收到事件后：
- `master`：`captureFlow: HANDSHAKING → IDLE`，绿色 toast「已识别为 Master」
- `slave`：红色 toast「请改连 Master 板」（不进入 IDLE，禁止录制）
- `unknown`：橘色 toast 但仍允许进入 IDLE（兼容旧 firmware）

### 4.3 单手 / 双手自动判别（[`parse_line`](LingxiGlove_Capture/src-tauri/src/serial_task.rs#L179-L249)）

**一行进、0~2 帧出**：

```rust
match values.len() {
    28 if dev_ts_ms != 0 => {
        // bimanual：13 master + 13 slave + slave_age + label
        let left  = Frame { dev_alias: "left",  values: master[0..13].to_vec(),
                            bimanual_raw: Some(line.to_string()), .. };
        let right = Frame { dev_alias: "right", values: slave[0..13].to_vec(),
                            raw_line: "", bimanual_raw: None, .. };
        return vec![left, right];
    }
    _ => vec![Frame { dev_alias: alias.clone(), raw_line: line.to_string(), .. }],
}
```

**判别条件**：`values.len() == 28 && dev_ts_ms != 0`
（必须有时间戳头列，否则当作 28 路纯通道行兜底）

**容错**：注释行 `#` `//`、空行、token 解析失败 → 返回空 Vec，aggregator 不会收到。
**单测**：[`parse_bimanual_29cols`](LingxiGlove_Capture/src-tauri/src/serial_task.rs#L286-L311) 用 0.1 / 0.9 标识 master/slave 各通道，断言拆分正确性。

### 4.4 FrameAggregator + 锁策略（[aggregator.rs](LingxiGlove_Capture/src-tauri/src/aggregator.rs)）

```rust
while let Some(mut frame) = rx.recv().await {
    frame.label = label_state.get();          // ① 注入全局 label
    fps_map.lock().tick(&frame.dev_alias);    // ② 帧率
    {                                          // ③ 写盘（锁尽量短）
        let mut sess = session.lock().unwrap();
        if let Some(s) = sess.as_mut() {
            s.on_frame_obj(&frame).ok();
        }
    } // 锁立即释放
    app.emit("frame", &frame);                // ④ 推前端
}
```

**为什么用 std `Mutex` 而不是 `tokio::Mutex`**？
- 临界区是「写一行 BufWriter」（无 await），耗时 < 0.1ms
- std::sync::Mutex 速度更快、零开销；tokio::Mutex 适合跨 await 的场景
- 不同分支可以读 `fps_map`、`session`、`label` 互不阻塞

**故障隔离**：写盘出错只 `log::error!`，不退出 aggregator。任一 SerialTask drop 后 `tx` 减一，
所有 SerialTask 都退出后 channel 关闭 → aggregator 自然退出。

### 4.5 Session 写盘 — 路由 + 行替换 + 兼容性铁律（[session.rs](LingxiGlove_Capture/src-tauri/src/session.rs)）

**目录结构**（与 `build_dataset.py` 字节级对齐）：

```
<out_root>/
├── session_20260527_103245_left/raw.csv         (单手 / Master 拆出的左路)
├── session_20260527_103245_right/raw.csv        (Master 拆出的右路)
└── session_20260527_103245_bimanual/raw.csv     (29 列联合，懒建)
```

`build_dataset.py` 用 `glob("session_*/raw.csv")` 枚举，**文件名必须严格 `raw.csv`**。

**`on_frame_obj` 路由表**：

| Frame 特征 | 写到哪 |
|---|---|
| `bimanual_raw = Some(...)` | bimanual writer（首次到达时懒建） |
| `raw_line` 非空 + `dev_alias` 在 writers | alias writer |
| `raw_line` 为空（拆出的右帧） | 静默丢弃（不重复写 13 列损失上下文） |

**label 列覆写**（[`substitute_last_label`](LingxiGlove_Capture/src-tauri/src/session.rs#L119-L129)）：

```rust
match raw.rfind(',') {
    Some(idx) => format!("{}{}", &raw[..=idx], label),
    None => raw.to_string(),  // 兜底
}
```

**为什么不解析 + 重新 format？** —— 端侧浮点格式（`%.4f` / `%d`）和 Rust `f32::to_string()` 字节不一致，
直接 `rfind(',')` 替换最后一列零格式偏差，规避「同一份原始数据上下游不字节相等」的兼容性灾难。

**flush 策略**：每 50 行（≈ 2.5s @ 20Hz）一次。崩溃时最多丢 2.5s。

### 4.6 LabelState — PC 端权威（[label.rs](LingxiGlove_Capture/src-tauri/src/label.rs)）

```rust
pub struct LabelState { label: Mutex<i8> }   // 默认 -1
```

链路：

```
键盘 0-9  →  set_label(n)  →  LabelState::set
                                    │
              每帧到 aggregator ───►│ frame.label = LabelState::get()
                                    │
              SessionWriter::write_frame  覆写最后一列
```

**为什么不用 atomic？** —— `i8` 用 `AtomicI8` 也行，但 `Mutex<i8>` 在意图表达上更清晰（"PC 端权威，串行更新"），
且 `set/get` 都是几纳秒，性能上没区别。

**前端 store** 在 `setCurrentLabel` 失败时**回滚**：再调一次 `getLabel` 拉后端真值，确保前后端一致。

### 4.7 采集状态机 captureFlow（[store.ts](LingxiGlove_Capture/src-tauri/src/store.ts)）

```
DISCONNECTED ── connect ──► HANDSHAKING ── role=master ──► IDLE
                                  │                          │ Space
                                  │ role=slave/timeout       ▼
                                  ▼                       COUNTDOWN (3-2-1-GO)
                              （阻塞）                       │ +3.1s
                                                             ▼
                                  Esc ◄────cancelCountdown───┤
                                                             │ sendChar 'c' + startSession
              Enter / Esc                                    ▼
              ◄── stopRecordingFlow ── FINISHING ◄──── RECORDING
                  (sendChar 'r' + stop_session)
```

**关键实现细节**：

1. **module-level 取消句柄**
   ```ts
   let countdownCancelImpl: (() => void) | null = null;
   ```
   `startCountdownFlow` 创建 cancel 闭包并赋值给该变量，
   `cancelCountdown` 跨 action 边界调用它清理 4 个 setTimeout。
   守卫：`startCountdownFlow` 第一行 `if (flow !== 'IDLE') return;` 防 ABA。

2. **键盘 / toolbar 共用同一组 actions**
   两条入口都直接调 `store.startCountdownFlow / stopRecordingFlow / cancelCountdown`，
   不存在「键盘自己一套 setTimeout 链 + toolbar 又一套」的双路径。

3. **GO 时的串行流**：
   ```ts
   await sendChar(MASTER_ALIAS, "c");           // 让端侧进入 capture 模式
   await new Promise(r => setTimeout(r, 200));  // 等端侧切换 + 第一行 CSV
   await get().startSession();                  // 创建 session 目录 + writer
   set({ captureFlow: "RECORDING", recordingStartedAt: performance.now() });
   ```
   200ms 等待是为了确保「端侧 c 命令切换 → 第一行 29 列 CSV 出来」之间不会有
   `[配置]` 日志被 SessionWriter 误吞（aggregator 在 IDLE 期间本就不写盘，
   双保险）。

### 4.8 全局键盘 hook（[useGlobalKeydown.ts](LingxiGlove_Capture/src/hooks/useGlobalKeydown.ts)）

**逻辑下沉到 store**，hook 只做 dispatch：

| 键 | 状态 | 动作 |
|---|---|---|
| `0-9` | 任何 | `setCurrentLabel(n)` |
| `-` | 任何 | `setCurrentLabel(-1)` |
| `Space` | IDLE | `startCountdownFlow()` |
| `Enter` | RECORDING | `stopRecordingFlow()` |
| `Esc` | COUNTDOWN | `cancelCountdown()` |
| `Esc` | RECORDING | `stopRecordingFlow()`（abort 但保留已写盘） |
| `?` | 任何 | toggle help overlay |

**输入框放行**：`isEditableTarget()` 检测 INPUT/TEXTAREA/SELECT/contentEditable 时直接 return，
配合 LabelHUD inline 编辑里的 `e.stopPropagation()`，确保编辑 label 名时数字 / 空格不会夺权。

### 4.9 uPlot 实时绘图（[RealtimePlot.tsx](LingxiGlove_Capture/src/components/RealtimePlot.tsx)）

**性能要点**：

1. **不进 React state** — 40 fps × 2 设备进 setState 必卡 UI
2. **useRef 双 ring buffer**：
   - `xsRef = [0, 1, ..., windowSize-1]` 固定刻度
   - `ysRef[chIdx][i]` per-channel 数据
3. **rAF 60fps tick**：每帧 `setData([xs, ...ys])` 整组替换（uPlot 推荐用法）
4. **listen("frame") 在组件内部** 直接 push ring，按 `dev_alias` 过滤
5. **关掉所有交互**：`cursor.show=false / legend.live=false / select.show=false / scales.y.auto=false`
6. **y 轴固定** `[0, 4096]`（flex 范围），不自适应避免抖动

通道选择默认绘 5 路 flex（阶段 1 主路径），可由 `channels` prop 覆盖。

### 4.10 Label 名持久化 + inline 双击编辑

**zustand + localStorage**（[store.ts loadLabelNames / persistLabelNames](LingxiGlove_Capture/src/store.ts#L66-L88)）：

```ts
const LABEL_STORAGE_KEY = "lingxi_capture_labels_v1";
// loadLabelNames: 容错回退 → 长度 / 类型不对都用 DEFAULT_LABEL_NAMES 兜底
// renameLabel(idx, name): trim + 校验 + 写 localStorage + set state
```

**LabelHUD 主面板双击 inline 编辑**（[LabelHUD.tsx](LingxiGlove_Capture/src/components/LabelHUD.tsx)）：

```tsx
const [editingIndex, setEditingIndex] = useState<number | null>(null);
const [draft, setDraft] = useState("");

<button onClick={() => !isEditing && setCurrentLabel(l)}
        onDoubleClick={(e) => { e.preventDefault(); beginEdit(l); }}>
  {l}. {isEditing
        ? <input ref={inputRef} value={draft}
                 onChange={e => setDraft(e.target.value)}
                 onKeyDown={e => {
                   e.stopPropagation();   // 防止全局快捷键夺权
                   if (e.key === "Enter") commitEdit();
                   if (e.key === "Escape") cancelEdit();
                 }}
                 onBlur={cancelEdit}
                 onClick={e => e.stopPropagation()} />
        : <em>{labelNames[l]}</em>}
</button>
```

`LabelEditor` 模态提供批量改名 + 重置默认；两条入口共享 `renameLabel` action。

### 4.11 数据流水线（[pipeline.rs](LingxiGlove_Capture/src-tauri/src/pipeline.rs)）

#### 4.11.1 子进程 build_dataset.py

```rust
let mut child = TokioCommand::new(&python)
    .arg(&script).arg("--in").arg(&in_root).arg("--out").arg(&out_root)
    .args(&extra_args)
    .stdout(Stdio::piped()).stderr(Stdio::piped())
    .spawn()?;

// 两个 task 各自 BufReader 按行读 → emit("pipeline-progress", { stage: "build_dataset_stdout", message })
tokio::spawn(stdout_pump);
tokio::spawn(stderr_pump);
let status = child.wait().await?;
emit("pipeline-progress", done(ok, exit_code));
```

**事件统一接口**：
```ts
{ stage: "build_dataset_stdout"|"build_dataset_done"|"upload_start"|"upload_progress"|"upload_done",
  message: string, current: number, total: number, ok: boolean | null }
```
前端 `PipelinePanel` listen 一次性消费，按 `stage` 分色显示。

#### 4.11.2 EI Ingestion 上传

```rust
let key = secrets::get_ei_key()?.ok_or("未配置 API Key")?;
for path in train_files.iter().chain(test_files.iter()) {
    let label = path.file_name().split('.').next();   // <label>.<seq>.csv
    let category = if from_train { "training" } else { "testing" };
    let resp = client.post(format!("https://ingestion.edgeimpulse.com/api/{}/data", category))
        .header("x-api-key", &key)
        .header("x-label", label)
        .multipart(form_with_csv)
        .send().await?;
    emit("pipeline-progress", upload_progress(idx, total));
}
```

#### 4.11.3 一键流水线

`run_pipeline = run_build_dataset.await + upload_to_ei.await`，
任一环节失败立即返回；UI 进度条用同一个事件流，无缝拼接。

### 4.12 Keychain 持久化（[secrets.rs](LingxiGlove_Capture/src-tauri/src/secrets.rs)）

```rust
keyring::Entry::new("lingxi-capture", "edge-impulse-api-key")
    .set_password(key) / get_password() / delete_credential()
```

平台映射：macOS = 登录钥匙串。重启应用、卸载重装都不丢失（除非用户在钥匙串 App 手动删）。
任何 keyring 错误都返回 `String`，前端 toast 展示，不 panic。

---

## 5. Tauri Command 全表

| Command | 入参 | 返回 | 说明 |
|---|---|---|---|
| `list_ports` | — | `SerialPortInfo[]` | macOS 自动过滤 `/dev/tty.*` |
| `connect_device` | `alias, port, baud` | `()` | spawn SerialTask + 注册 cancel/write 通道 |
| `disconnect_device` | `alias` | `()` | watch channel 取消 |
| `serial_write_byte` | `alias, byte` | `()` | 前端发 c/r/i 等控制字符 |
| `get_devices` | — | `DeviceMeta[]` | 含 status |
| `get_fps` | — | `FpsSnapshot[]` | 1Hz 滑窗 |
| `set_label` | `label: i8` | `()` | LabelState 写入 |
| `get_label` | — | `i8` | 拉权威值 |
| `start_session` | — | `session_id: String` | 创建目录 + writers |
| `pause_session` | — | `()` | 不丢 writer，frame 仍 emit 不写盘 |
| `resume_session` | — | `()` | — |
| `stop_session` | — | `SessionSummary` | flush + 返回 per-device 摘要 |
| `get_session_info` | — | `SessionInfo` | recording / paused / rows_per_device |
| `get_out_root` | — | `String` | `<app_data>/output/capture` |
| `list_sessions` | `out_root` | `SessionEntry[]` | session_*/raw.csv 枚举 |
| `run_build_dataset` | `BuildDatasetArgs` | `ProcessOutcome` | 异步 + emit 进度 |
| `upload_to_ei` | `dataset_root` | `UploadOutcome` | multipart × N |
| `run_pipeline` | `BuildDatasetArgs` | `PipelineOutcome` | build → upload 串联 |
| `set_ei_key` / `has_ei_key` / `delete_ei_key` | — / — / — | `()` / `bool` / `()` | Keychain |

---

## 6. 事件全表（Rust → 前端）

| 事件名 | 触发 | payload |
|---|---|---|
| `frame` | aggregator 每收一帧 | `Frame` |
| `serial-role` | role 检测命中 / 5s 兜底 | `{ alias, role: "master" \| "slave" \| "unknown" }` |
| `pipeline-progress` | build/upload 子任务每行 / 每文件 | `{ stage, message, current, total, ok }` |

---

## 7. 兼容性铁律 — 与 build_dataset.py 0 修改对接

| 项 | 锁定 |
|---|---|
| 目录格式 | `session_<YYYYMMDD_HHMMSS>_<alias>/raw.csv` 严格匹配 `glob("session_*/raw.csv")` |
| 文件名 | 必须是 `raw.csv`，不能 `raw_left.csv` 等 |
| 单手 header | `timestamp_ms,ax,ay,az,gx,gy,gz,pitch,roll,flex0..4,label` 共 15 列 |
| 双手 header | `timestamp_ms,m_(...),s_(...),slave_age_ms,label` 共 29 列 |
| label 列位置 | **永远是最后一列**，PC 端用 `rfind(',')` 替换 |
| label 取值 | -1 / 0..9 i8，与端侧 `CAPTURE_LABEL_NAMES[]` 对齐 |

每次改这部分前先跑：
```bash
python ../LingxiGlove/tools/build_dataset.py \
    --in <app_data>/output/capture --out /tmp/check
```
0 修改通过为合格。

---

## 8. 验收快照（Day 5）

| 项 | 命令 | 期望 |
|---|---|---|
| Rust 编译 | `cargo check` | 0 error |
| Rust 单测 | `cargo test --lib` | 16/16 passed（含 bimanual / arbitrator / esp_now_sync 等） |
| TS 类型 | `npx tsc --noEmit` | 0 error |
| 前端构建 | `npx vite build` | dist 输出 ≈ 250 KB |
| 端到端 | 接 Master + 空格 | 3-2-1-GO + 写 `session_*/raw.csv` |
| 兼容性 | `build_dataset.py` 消费 | 0 修改通过 |

---

## 9. Day 5 之后的展望

短期（按优先级）：
- 真机端到端实测（接板子跑一次完整 5 类 × 10 样本）
- `npm run tauri build` 出 .dmg，README 进度看板收尾
- 手语动作素材库（[docs/手语动作素材与被测者训练方案.md](LingxiGlove_Capture/docs/%E6%89%8B%E8%AF%AD%E5%8A%A8%E4%BD%9C%E7%B4%A0%E6%9D%90%E4%B8%8E%E8%A2%AB%E6%B5%8B%E8%80%85%E8%AE%AD%E7%BB%83%E6%96%B9%E6%A1%88.md)）
  方案 A：LabelEditor 加 mediaPath 字段，倒计时前 2s 预览动作示范

中长期（PLAN.md §7）：3D 手套实时镜像 / 实时模型在线推理 / Webcam 同步录像 / Windows 移植 /
数据增强预览 / 采集 recipe 脚本化。

---

## 10. 启动方式

```bash
cd /Users/kun.li/Code/Lingxi/LingxiGlove_Capture
. "$HOME/.cargo/env"     # 让本会话 PATH 看到 cargo（首次 / 新 shell）
npm install              # 首次
npm run tauri dev        # 开发模式（hot reload）
npm run tauri build      # 出 .dmg
```

详见 [README.md](LingxiGlove_Capture/README.md)
和 [docs/KEYBINDINGS.md](LingxiGlove_Capture/docs/KEYBINDINGS.md)。
