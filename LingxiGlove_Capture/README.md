# LingxiGlove Capture

> 灵犀手套 — 跨平台数据采集与打标桌面应用（macOS 首发）

## 项目定位

**取代** [LingxiGlove/tools/capture_serial.py](../LingxiGlove/tools/capture_serial.py) +
手动数字键打标 + [build_dataset.py](../LingxiGlove/tools/build_dataset.py) 的多步 CLI 流程，
做成一站式 GUI。

**双设备并行**：原生支持左右手两块板子同时采集，时间对齐 + 统一打标。

**端到端闭环**：从串口采集 → 实时可视化 → 打标 → 切窗 → 一键直传 Edge Impulse。

## 技术栈（已锁定）

| 层 | 选型 | 备注 |
|---|---|---|
| 框架 | **Tauri 2.x** | 包体 <15MB，原生性能 |
| Rust 串口 | **tokio-serial** | 异步多串口并行 |
| Rust HTTP | **reqwest** + multipart | EI Ingestion API |
| Rust 凭据 | **keyring** | macOS Keychain 直通 |
| 前端 | **React + TypeScript + Vite** | 复用 LingxiGlove_APP 同栈 |
| 状态管理 | **zustand** | 轻量，不引入 redux |
| 样式 | **Tailwind CSS** | 复用 LingxiGlove_APP 主题 |
| 实时绘图 | **uPlot** | 60fps 不掉帧的唯一选择 |
| 切窗 | **Python subprocess** 调 build_dataset.py | 不重写，零 bug |

## 目录结构（规划）

```
LingxiGlove_Capture/
├── README.md                  本文件
├── PLAN.md                    4 天冲刺计划 + 任务拆分
├── docs/
│   ├── ARCHITECTURE.md        架构详解（Day 1 补完）
│   ├── KEYBINDINGS.md         键盘快捷键参考
│   └── EI_INGESTION.md        Edge Impulse Ingestion API 集成笔记
├── src/                       前端源码（Day 1 起）
├── src-tauri/                 Rust 后端（Day 1 起）
├── output/                    本地采集数据（运行时生成，.gitignore）
└── package.json / Cargo.toml  构建配置
```

## 快速上手（开发阶段）

```bash
# 1. 安装 Tauri 工具链（首次）
cargo install create-tauri-app --locked
brew install rustup-init && rustup-init -y

# 2. 进入项目（Day 1 起可用）
cd LingxiGlove_Capture

# 3. 开发模式
npm install
npm run tauri dev

# 4. 打包 dmg
npm run tauri build
# 产物：src-tauri/target/release/bundle/dmg/*.dmg
```

## 快速上手（用户使用）

1. 双击 `.dmg` 安装，从 Applications 启动
2. **首次启动会自动弹出 5 步入门引导**（也可点 header 的「入门」随时复习）
3. **顶部 ConnectBar** 选择串口（下拉自动列出 `/dev/cu.usbmodem*`），点「连接」
4. **左/右** 两条 ConnectBar 分别连接左右手手套（单手训练只连一条也可）
5. **空格** 开始/暂停录制；**数字键 0-9** 切换标签；**`-`** 复位为 unlabeled；**回车** 停止
6. 滚到底部「数据流水线」面板：
   - 设置卡片：填 `build_dataset.py` 路径 / Python 解释器 / EI API Key（一次性，存 macOS Keychain）
   - 运行卡片：点「① build_dataset」切窗，再「② upload_to_ei」上传，或直接「🚀 一键流水线」
7. 全部输出实时打到日志区，stage 配色 + 自动滚动

> 完整快捷键参考：[docs/KEYBINDINGS.md](docs/KEYBINDINGS.md)
> 架构与数据流：[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)
> 4 天冲刺日志：[PLAN.md](PLAN.md)

## 进度（截至 Day 4）

- [x] **Day 1** 双串口数据流骨架 + uPlot 双面板
- [x] **Day 2** 实时绘图 + 打标 + 会话落盘 + 摘要 toast
- [x] **Day 3** PipelinePanel + build_dataset 子进程 + EI Ingestion 上传 + Keychain
- [x] **Day 4** 首启 5 步引导 + 关于页 + 底栏快捷键提示 + KEYBINDINGS.md
- [x] **Day 4** `tauri build` 出 dmg（产物 `src-tauri/target/release/bundle/dmg/lingxi-capture_0.1.0_aarch64.dmg`，5.5 MB）
- [ ] **Day 2 / 3 端到端实测**（需硬件 + EI 账号）

## 双设备并行核心设计

```
            ┌──────────────┐         ┌──────────────┐
            │ Left Glove   │         │ Right Glove  │
            │ (Master)     │         │ (Slave)      │
            └──────┬───────┘         └──────┬───────┘
                   │ USB                    │ USB
                   ▼                        ▼
            ┌──────────────┐         ┌──────────────┐
            │ tokio task L │         │ tokio task R │
            │ (read loop)  │         │ (read loop)  │
            └──────┬───────┘         └──────┬───────┘
                   │ mpsc channel           │
                   └──────────┬─────────────┘
                              ▼
                   ┌─────────────────────┐
                   │ FrameAggregator     │
                   │ - 打 PC recv_ts     │
                   │ - 统一应用 label    │
                   │ - 写入两个 raw.csv  │
                   └──────────┬──────────┘
                              │ Tauri event
                              ▼
                   ┌─────────────────────┐
                   │ React UI (uPlot×2)  │
                   └─────────────────────┘
```

**核心原则**：
- **时钟对齐**：以 PC `SystemTime::now()` 为基准，不信任设备 `millis()`
- **打标统一**：按键事件在 PC 端广播到所有活跃 session，左右手 label 完全同步
- **故障隔离**：一个设备掉线，另一个继续采集，UI 红色标识掉线方
- **文件独立**：每个设备一份 `raw_<dev_alias>.csv`，下游脚本兼容性 100%

## 与现有工程的边界

| 现有 | 在本工具中的处置 |
|---|---|
| [LingxiGlove/tools/capture_serial.py](../LingxiGlove/tools/capture_serial.py) | **保留**作为 CLI 兜底 |
| [LingxiGlove/tools/build_dataset.py](../LingxiGlove/tools/build_dataset.py) | **复用**（subprocess 调用） |
| 端侧 MODE_CAPTURE 协议 | **不动**，本工具只是消费方 |
| `output/capture/session_*/` 目录格式 | **不动**，兼容铁律 |

## 启动前置条件（铁律）

主路径核心识别（5 路 flex + 双手 + 10 类准确率 ≥ 90%）必须先达标，
否则禁止启动本工具开发——避免占用主线资源。

详见 [LingxiGlove/doc/DEVELOPMENT.md §11](../LingxiGlove/doc/DEVELOPMENT.md)。

## 详细计划

见 [PLAN.md](PLAN.md)。
