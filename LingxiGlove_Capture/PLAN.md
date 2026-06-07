# LingxiGlove Capture — 实施计划（4 天冲刺 · macOS-only · 双设备并行）

> **版本**：v1.0（2026-05-28）
> **范围**：M1 MVP，含双手板并行采集
> **预算**：4 个全工作日（单人 fulltime）
> **总目标**：从启动 app → EI 后台看到双手对齐的样本数据 < 5 分钟

---

## 0. 范围裁剪与铁律

### 0.1 已锁定的减法

| 项 | 决策 |
|---|---|
| 串口选择 | 手动下拉，**不做**自动检测/热插拔 |
| 平台 | **仅 macOS**，Windows 推迟 |
| 绘图库 | **uPlot**（不评估 D3/Recharts） |
| 切窗 | **subprocess** 调现有 `build_dataset.py`，不重写 |
| 应用签名 | **不签名**，用户右键打开 |

### 0.2 铁律（不可妥协）

1. **CSV 兼容性 100%**：raw_*.csv 必须能被现有 `build_dataset.py` 直接消费，零修改
2. **打标响应 < 50ms**：按键到 UI 反馈
3. **双设备时钟对齐误差 < 50ms**：PC recv_ts 作为统一时间轴
4. **单天超时 > 2h** 当天不完成的延后，**不熬夜赶**
5. **主路径未达标禁止开工**：核心识别（5 路双手 10 类 ≥ 90%）必须先达标

---

## 1. 架构核心（双设备 + 时钟对齐）

### 1.1 数据流总览

```
设备  Left Glove ───┐                 Right Glove ───┐
        USB         │                     USB        │
                    ▼                                ▼
Rust    SerialTask::run(path, alias)    SerialTask::run(path, alias)
        │ 解析 CSV 行                   │ 解析 CSV 行
        │ 打 PC recv_ts                 │ 打 PC recv_ts
        ▼                               ▼
        mpsc::Sender<Frame> ──────────► FrameAggregator
                                        │
                                        ├─► SessionWriter (写 raw_left.csv)
                                        ├─► SessionWriter (写 raw_right.csv)
                                        ├─► LabelBroadcaster (统一打标)
                                        └─► UI Emitter (Tauri emit)
                                                       │
前端                                                   ▼
        zustand store ◄──── 解析 frame ── on_event("frame")
        │                                              │
        ├─► RealtimePlot (uPlot × 2 实例)              │
        ├─► LabelHUD                                   │
        └─► SessionPanel                               │
```

### 1.2 关键数据结构（Rust）

```rust
// src-tauri/src/types.rs
pub struct Frame {
    pub dev_alias: String,        // "left" | "right"
    pub recv_ts_ms: u64,          // PC 系统时间戳（全局基准）
    pub dev_ts_ms: u32,           // 设备 millis（仅参考）
    pub label: i8,                // 当前 label（由 PC 端注入）
    pub values: Vec<f32>,         // 解析后的传感器数值
    pub raw_line: String,         // 原始 CSV 行（便于落盘）
}

pub struct Session {
    pub id: String,                       // session_<时间戳>
    pub started_at: SystemTime,
    pub devices: HashMap<String, DeviceMeta>,  // alias -> meta
    pub current_label: i8,
    pub writers: HashMap<String, BufWriter<File>>,  // alias -> raw_*.csv
}
```

### 1.3 时钟对齐策略

- **永远以 `SystemTime::now()` 作为统一时间轴**
- 写入 `raw_*.csv` 时把首列 `timestamp_ms` 替换为 `recv_ts_ms`（保持列名不变，下游脚本无感）
- 设备的 `dev_ts_ms` 单独留 1 列 `dev_ts` 作参考（可选，不破坏兼容性）
- 双设备的对齐由下游切窗时按 recv_ts 窗口匹配完成

### 1.4 打标统一机制

- 前端按键 → invoke `set_label(label: i8)`
- Rust `LabelBroadcaster` 立即将 `current_label` 写入所有活跃 session
- 后续每个 Frame 出现时都会带上当前 label
- **左右手 label 完全同步**，不需要分别打

### 1.5 故障隔离

| 场景 | 处理 |
|---|---|
| 一个串口断开 | 红色 badge + 自动停该设备 writer，另一设备继续 |
| 串口数据格式错误 | 跳过该行，错误计数器 +1，UI 角标红色 |
| 切到错误串口 | 一直无数据时，10 秒后弹提示 "未检测到 CSV header" |

---

## 2. 键盘交互设计

### 2.1 全局快捷键

| 键 | 动作 | 备注 |
|---|---|---|
| `Space` | **开始/暂停** 录制 | 核心操作 |
| `Enter` | 结束当前 session，保存元数据 | 弹确认框 |
| `0-9` | 切换 label | 立即在 UI 大字号高亮 |
| `-` | 复位为 unlabeled | 数据丢弃 |
| `Tab` | 焦点切换"左/右"主控面板 | — |
| `R` | 重新扫描串口 | — |
| `?` | 呼出帮助弹窗 | 内含完整快捷键 |
| `Esc` | 关闭弹窗 | — |

### 2.2 内置使用说明

- **首次启动**：自动弹出 5 步引导（每步带截图占位）
- **常驻底栏**：单行提示当前可用操作，如 `空格 录制 · 0-9 标签 · 回车 结束 · ? 帮助`
- **帮助弹窗 `?`**：完整快捷键表 + 双设备连接示意图

---

## 3. 4 天逐日任务拆分

### Day 1 — 项目骨架 + 双串口数据流

#### 上午（4h）

| 任务 | 交付物 | 验收 |
|---|---|---|
| T1.1 Tauri 脚手架 | `npm create tauri-app@latest` React+TS 模板 | `npm run tauri dev` 能开窗 |
| T1.2 依赖安装 | Cargo.toml 加 tokio-serial / serde / reqwest / keyring；package.json 加 zustand / uplot / tailwind | `cargo check` 通过 |
| T1.3 Rust types.rs | Frame / Session / DeviceMeta 结构定义 | `cargo check` 通过 |
| T1.4 Tauri command: list_ports | 调 `tokio_serial::available_ports()` | 前端 invoke 返回 `/dev/cu.usbmodem*` |

#### 下午（4h）

| 任务 | 交付物 | 验收 |
|---|---|---|
| T1.5 SerialTask::run | 单设备 tokio 任务，逐行读串口，跳过 `[` 开头日志 | 单串口能持续输出帧到 channel |
| T1.6 FrameAggregator | mpsc 接收多源 Frame，打 recv_ts | console 看到双源数据交织 |
| T1.7 UI 顶部连接栏 | 左右两个独立连接区，各自 [串口▾][波特率▾][连接] | 双串口能分别连/断 |
| T1.8 状态栏 | 各设备实时帧率 + 连接状态 | 双源都显示 ~20 fps |

**Day 1 总验收**：双手板同时连接，console 看到 `[Left] frame n=...` 和 `[Right] frame n=...` 交织输出，帧率稳定。

---

### Day 2 — 实时折线图 + 打标 + CSV 落盘

#### 上午（4h）

| 任务 | 交付物 | 验收 |
|---|---|---|
| T2.1 uPlot 封装 RealtimePlot | 滚动窗口 200 点，useRef 缓冲，rAF 60fps 拉 | 单 plot 流畅滚动 |
| T2.2 双面板布局 | 左右垂直分屏，各自一个 RealtimePlot | 双设备同时绘图无掉帧 |
| T2.3 通道开关 | 复选框控制 series 显隐 | 切换即时生效 |
| T2.4 y 轴固定 + auto:false | 防止抖动 | 视觉稳定 |

#### 下午（4h）

| 任务 | 交付物 | 验收 |
|---|---|---|
| T2.5 zustand store | currentLabel / isRecording / sessions | DevTools 看状态变更 |
| T2.6 全局 keydown 监听 | Space / Enter / 0-9 / `-` | 按键触发对应 store action |
| T2.7 LabelHUD 组件 | 右上角大字号 + 颜色编码 | 切 label < 50ms 反馈 |
| T2.8 SessionWriter | Rust 写 raw_<alias>.csv + meta.json | 文件出现在 output/session_*/ |
| T2.9 兼容性验证 | 跑 `python ../LingxiGlove/tools/build_dataset.py --in output/capture` | **无报错通过**（铁律） |

**Day 2 总验收**：双手板各 30 秒采集，输出两个 raw_*.csv，能被 build_dataset.py 直接消费。

---

### Day 3 — 切窗 + EI 直传 + 一键流水线

#### 上午（4h）

| 任务 | 交付物 | 验收 |
|---|---|---|
| T3.1 SessionPanel 左侧栏 | 列出 output/session_*，单击展开 meta | 能浏览历史 |
| T3.2 切窗 invoke | tauri-plugin-shell 调 python3 build_dataset.py | UI 进度条显示 stdout |
| T3.3 EI Ingestion API | reqwest multipart 上传 ei_csv/train/*.csv | EI 后台能看到数据 |
| T3.4 API Key 存储 | keyring crate 写 macOS Keychain | 重启 app 不丢失 |

#### 下午（4h）

| 任务 | 交付物 | 验收 |
|---|---|---|
| T3.5 一键流水线按钮 | 选 session → [切窗] → [上传] 串行 | 全程进度条 + 失败重试 |
| T3.6 设置弹窗 | EI API Key 输入 + 项目 ID + 默认窗口参数 | 配置可保存 |
| T3.7 错误处理 | 串口断开 / EI 401 / 切窗失败 | toast 提示 + 日志面板 |
| T3.8 单元自测 | 端到端跑 1 个完整会话 | 端到 EI 后台 < 5 分钟 |

**Day 3 总验收**：完整流水线打通，可上传到 EI 测试项目。

---

### Day 4 — 帮助文档 + 打包 + 优化

#### 上午（4h）

| 任务 | 交付物 | 验收 |
|---|---|---|
| T4.1 首启引导 | 5 步弹窗（连串口/选 label/录制/结束/上传） | 首次启动自动触发 |
| T4.2 帮助弹窗 `?` | 完整快捷键 + 故障排查 | 任意时刻 `?` 呼出 |
| T4.3 底栏常驻提示 | 当前可用操作动态切换 | 上下文相关 |
| T4.4 docs/KEYBINDINGS.md | 完整快捷键参考 | 链接到 README |

#### 下午（4h）

| 任务 | 交付物 | 验收 |
|---|---|---|
| T4.5 应用图标 | 1024×1024 PNG，复用 LingxiGlove logo | dmg 显示自定义图标 |
| T4.6 关于页面 | 版本号 / Git commit / 致谢 | `?` → About |
| T4.7 `npm run tauri build` | 出 .dmg | 双击安装从 Applications 启动 |
| T4.8 README + 录屏 | 安装/使用动图（gif） | 新人 3 分钟上手 |
| T4.9 双设备压测 | 连续采集 10 分钟无 crash | 帧率稳定 / 内存平稳 |

**Day 4 总验收**：.dmg 分发包就绪，README 完整，团队任意成员能 3 分钟上手。

---

## 4. 文件结构（Day 4 完成时）

```
LingxiGlove_Capture/
├── README.md
├── PLAN.md
├── docs/
│   ├── ARCHITECTURE.md       Day 1 完成后补
│   ├── KEYBINDINGS.md        Day 4
│   └── EI_INGESTION.md       Day 3
├── src/
│   ├── App.tsx
│   ├── components/
│   │   ├── ConnectionBar.tsx
│   │   ├── DevicePanel.tsx          // 左/右单设备面板
│   │   ├── RealtimePlot.tsx         // uPlot 封装
│   │   ├── LabelHUD.tsx
│   │   ├── SessionPanel.tsx
│   │   ├── StatusBar.tsx
│   │   ├── HelpDialog.tsx
│   │   └── SettingsDialog.tsx
│   ├── store/
│   │   ├── deviceStore.ts
│   │   ├── sessionStore.ts
│   │   └── labelStore.ts
│   ├── hooks/
│   │   ├── useSerialFrames.ts       // Tauri event 订阅
│   │   ├── useHotkeys.ts
│   │   └── useFrameRate.ts
│   └── styles/
│       └── tailwind.css
├── src-tauri/
│   ├── Cargo.toml
│   ├── tauri.conf.json
│   └── src/
│       ├── main.rs
│       ├── types.rs
│       ├── serial_task.rs            // 每串口一个 tokio 任务
│       ├── aggregator.rs             // FrameAggregator
│       ├── session.rs                // SessionWriter
│       ├── label.rs                  // LabelBroadcaster
│       ├── commands/
│       │   ├── ports.rs              // list_ports / open / close
│       │   ├── session.rs            // start / stop / list
│       │   ├── label.rs              // set_label
│       │   ├── window.rs             // run build_dataset.py
│       │   └── ei.rs                 // upload_to_ei
│       └── ei/
│           ├── api.rs                // Ingestion HTTP
│           └── keychain.rs           // keyring 封装
├── output/                            // .gitignore
└── package.json / Cargo.toml / vite.config.ts
```

---

## 5. 风险拦截

| 风险 | 拦截动作 | 触发条件 |
|---|---|---|
| Tokio + tauri-serial 集成踩坑 | 退路用 sync `serialport` crate + std::thread | Day 1 下午仍无数据 |
| uPlot 双 plot 性能掉帧 | 降级 canvas 自绘 1 路简版 | Day 2 上午 fps < 40 |
| Python subprocess 跨进程编码问题 | 改为 Rust 重写最简切窗（仅单通道） | Day 3 上午切窗不通 |
| EI Ingestion API 文档不全 | 砍掉云端，只交付本地 MVP，EI 上传保留 CLI | Day 3 下午仍 401/403 |
| 单天超时 > 2h | 砍掉当日 P1 任务，延次日 | 每日 6pm 自检 |

---

## 6. 验收清单（Day 4 收尾）

- [ ] 双手板同时连接，各自实时绘图无掉帧
- [ ] 空格录制、回车结束、0-9 切 label 全部 < 50ms 响应
- [ ] 一次完整会话产出 `raw_left.csv` + `raw_right.csv` + `meta.json` + `labels.csv`
- [ ] 现有 `build_dataset.py` 能直接消费两个 raw 文件
- [ ] EI 后台能看到上传的样本，label 正确
- [ ] `?` 帮助弹窗显示完整快捷键
- [ ] 首启引导自动触发
- [ ] `.dmg` 双击安装，从 Applications 启动
- [ ] 双设备压测 10 分钟无 crash，内存增长 < 50MB
- [ ] README + 录屏齐全

---

## 7. 后续迭代（不在本次 4 天内）

| 模块 | 预算 |
|---|---|
| 3D 手套实时镜像（复用 LingxiGlove_APP Three.js） | +1.5 天 |
| 实时模型在线推理（边采边显示预测） | +2 天 |
| Webcam 同步录像 | +1 天 |
| Windows 移植 | +2 天 |
| 数据增强预览 | +1 天 |
| 采集 recipe 脚本化 | +1 天 |

详见 [../LingxiGlove/doc/DEVELOPMENT.md §11](../LingxiGlove/doc/DEVELOPMENT.md)。
