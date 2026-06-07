//! Tauri command 接口
//!
//! 前端通过 `invoke('xxx')` 调用。Day 1 提供：
//! - `list_ports`：枚举系统串口
//! - `connect_device`：以 (alias, port, baud) 启动 SerialTask
//! - `disconnect_device`：触发 cancel
//! - `get_fps`：拉取 per-device 帧率快照
//! - `get_devices`：列出当前已连接设备
//!
//! 错误一律返回 `String`（前端 toast 直接展示）。

use crate::aggregator::{FpsMap, SharedSession};
use crate::app_config::AppConfig;
use crate::label::SharedLabelState;
use crate::serial_task::SerialTask;
use crate::session::{SessionState, SessionSummary};
use crate::types::{DeviceMeta, DeviceStatus, Frame, SerialPortInfo};
use std::collections::HashMap;
use std::path::PathBuf;
use std::sync::{Arc, Mutex};
use tauri::Manager;
use tokio::sync::{mpsc, watch};

/// 全局应用状态（被 lib.rs 注入到 Tauri State）
pub struct AppState {
    /// 所有 SerialTask 共享的 frame 发送端（克隆给每个 task）
    pub frame_tx: mpsc::Sender<Frame>,
    /// per-device 取消信号 sender
    pub cancellers: Arc<Mutex<HashMap<String, watch::Sender<bool>>>>,
    /// per-device 串口写命令通道（前端 → SerialTask 写串口字节）
    pub writers: Arc<Mutex<HashMap<String, mpsc::Sender<Vec<u8>>>>>,
    /// 已连接设备元数据
    pub devices: Arc<Mutex<HashMap<String, DeviceMeta>>>,
    /// 帧率统计
    pub fps_map: FpsMap,
    /// 当前 label（全局）
    pub label_state: SharedLabelState,
    /// 当前录制会话（None 表示未录制）
    pub session: SharedSession,
    /// 输出根目录（默认 <app_data>/output/capture，用户可在前端通过 set_out_root 修改）
    ///
    /// 用 Arc<Mutex<...>> 是为了允许运行时修改：用户在前端「会话列表」点击
    /// 修改按钮选择新目录后，立即生效（下次 start_session 写入新路径）。
    pub out_root: Arc<Mutex<PathBuf>>,
}


/// 枚举系统串口
#[tauri::command]
pub fn list_ports() -> Result<Vec<SerialPortInfo>, String> {
    let ports = serialport::available_ports().map_err(|e| format!("enumerate ports: {}", e))?;
    let mut out: Vec<SerialPortInfo> = ports
        .into_iter()
        .map(|p| {
            let (desc, ptype) = match &p.port_type {
                serialport::SerialPortType::UsbPort(info) => {
                    let d = info
                        .product
                        .clone()
                        .or(info.manufacturer.clone())
                        .unwrap_or_default();
                    (Some(d), "USB".to_string())
                }
                serialport::SerialPortType::BluetoothPort => (None, "Bluetooth".to_string()),
                serialport::SerialPortType::PciPort => (None, "PCI".to_string()),
                serialport::SerialPortType::Unknown => (None, "Unknown".to_string()),
            };
            SerialPortInfo {
                port_name: p.port_name,
                description: desc,
                port_type: ptype,
            }
        })
        // macOS 下同一物理设备会同时出现 /dev/tty.* 和 /dev/cu.*；
        // 数据采集习惯用 cu.*（callout，立即发送，不等待 carrier）
        .filter(|p| !p.port_name.starts_with("/dev/tty."))
        .collect();

    out.sort_by(|a, b| a.port_name.cmp(&b.port_name));
    Ok(out)
}

/// 连接设备
#[tauri::command]
pub async fn connect_device(
    alias: String,
    port: String,
    baud: u32,
    app: tauri::AppHandle,
    state: tauri::State<'_, AppState>,
) -> Result<(), String> {
    // 先 disconnect 同 alias 的旧 task（防止用户重连）
    {
        let mut canc = state.cancellers.lock().unwrap();
        if let Some(tx) = canc.remove(&alias) {
            let _ = tx.send(true);
        }
    }
    {
        let mut wm = state.writers.lock().unwrap();
        wm.remove(&alias);
    }

    let (cancel_tx, cancel_rx) = watch::channel(false);
    let (write_tx, write_rx) = mpsc::channel::<Vec<u8>>(32);
    let task = SerialTask {
        alias: alias.clone(),
        port: port.clone(),
        baud,
        tx: state.frame_tx.clone(),
        cancel: cancel_rx,
        write_rx,
        app_handle: app.clone(),
    };

    // 注册 device meta（先标 Connected，失败时再回退）
    {
        let mut devs = state.devices.lock().unwrap();
        devs.insert(
            alias.clone(),
            DeviceMeta {
                alias: alias.clone(),
                port: port.clone(),
                baud,
                status: DeviceStatus::Connected,
                frame_count: 0,
            },
        );
    }
    {
        let mut canc = state.cancellers.lock().unwrap();
        canc.insert(alias.clone(), cancel_tx);
    }
    {
        let mut wm = state.writers.lock().unwrap();
        wm.insert(alias.clone(), write_tx);
    }

    // 探测一下：先开后关，确认 port 可打开
    {
        let probe = tokio_serial::new(&port, baud).open();
        if let Err(e) = probe {
            // 回滚
            state.devices.lock().unwrap().remove(&alias);
            state.cancellers.lock().unwrap().remove(&alias);
            state.writers.lock().unwrap().remove(&alias);
            return Err(format!("打开串口失败：{}", e));
        }
        drop(probe);
    }

    // 启动后台 task；运行错误只 log，避免吞掉前端 dev console
    let alias_for_task = alias.clone();
    let devices = state.devices.clone();
    tauri::async_runtime::spawn(async move {
        if let Err(e) = task.run().await {
            log::error!("[{}] task exit with err: {}", alias_for_task, e);
            if let Ok(mut d) = devices.lock() {
                if let Some(m) = d.get_mut(&alias_for_task) {
                    m.status = DeviceStatus::Error;
                }
            }
        } else if let Ok(mut d) = devices.lock() {
            if let Some(m) = d.get_mut(&alias_for_task) {
                m.status = DeviceStatus::Idle;
            }
        }
    });

    Ok(())
}

/// 断开设备
#[tauri::command]
pub async fn disconnect_device(
    alias: String,
    state: tauri::State<'_, AppState>,
) -> Result<(), String> {
    let tx = {
        let mut canc = state.cancellers.lock().unwrap();
        canc.remove(&alias)
    };
    if let Some(tx) = tx {
        let _ = tx.send(true);
    }
    {
        let mut wm = state.writers.lock().unwrap();
        wm.remove(&alias);
    }
    {
        let mut devs = state.devices.lock().unwrap();
        if let Some(m) = devs.get_mut(&alias) {
            m.status = DeviceStatus::Idle;
        }
    }
    Ok(())
}

/// 向指定 alias 的串口写入若干字节（前端按键命令通路：c/r/i 等）
#[tauri::command]
pub async fn serial_write_byte(
    alias: String,
    bytes: Vec<u8>,
    state: tauri::State<'_, AppState>,
) -> Result<(), String> {
    let tx = {
        let wm = state.writers.lock().unwrap();
        wm.get(&alias).cloned()
    };
    match tx {
        Some(tx) => tx
            .send(bytes)
            .await
            .map_err(|e| format!("write channel closed: {}", e)),
        None => Err(format!("设备 {} 未连接", alias)),
    }
}

/// 获取设备列表（含状态）
#[tauri::command]
pub fn get_devices(state: tauri::State<'_, AppState>) -> Vec<DeviceMeta> {
    state
        .devices
        .lock()
        .unwrap()
        .values()
        .cloned()
        .collect()
}

/// 帧率快照
#[derive(serde::Serialize)]
pub struct FpsSnapshot {
    pub alias: String,
    pub frame_count: u64,
    pub fps: f32,
}

#[tauri::command]
pub fn get_fps(state: tauri::State<'_, AppState>) -> Vec<FpsSnapshot> {
    state
        .fps_map
        .lock()
        .unwrap()
        .iter()
        .map(|(k, v)| FpsSnapshot {
            alias: k.clone(),
            frame_count: v.frame_count,
            fps: v.fps,
        })
        .collect()
}

// ---------------- Day 2: 打标 + 录制会话 ----------------

/// 设置全局当前 label（前端 keydown 调用）
///
/// 约定：-1 = unlabeled，0..=9 = 与端侧 `CAPTURE_LABEL_NAMES[]` 对齐
#[tauri::command]
pub fn set_label(label: i8, state: tauri::State<'_, AppState>) -> Result<(), String> {
    if !(-1..=99).contains(&label) {
        return Err(format!("label 越界：{}（允许 -1..=99）", label));
    }
    state.label_state.set(label);
    Ok(())
}

/// 获取当前全局 label
#[tauri::command]
pub fn get_label(state: tauri::State<'_, AppState>) -> i8 {
    state.label_state.get()
}

/// 当前会话状态快照（前端拉一次或事件订阅都行，MVP 走前端 1Hz 轮询）
#[derive(serde::Serialize)]
pub struct SessionInfo {
    pub recording: bool,
    pub paused: bool,
    pub session_id: Option<String>,
    /// 各设备已写入行数
    pub rows_per_device: HashMap<String, u64>,
}

#[tauri::command]
pub fn get_session_info(state: tauri::State<'_, AppState>) -> SessionInfo {
    let guard = state.session.lock().unwrap();
    match guard.as_ref() {
        Some(s) => {
            let mut rows = HashMap::new();
            for (alias, w) in s.writers.iter() {
                rows.insert(alias.clone(), w.rows_written());
            }
            SessionInfo {
                recording: true,
                paused: s.paused,
                session_id: Some(s.session_id.clone()),
                rows_per_device: rows,
            }
        }
        None => SessionInfo {
            recording: false,
            paused: false,
            session_id: None,
            rows_per_device: HashMap::new(),
        },
    }
}

/// 开始录制会话
///
/// - 自动收集**当前已连接**的设备 alias 列表（来自 AppState.devices 中 status=Connected）
/// - 为每个 alias 创建 `<out_root>/session_<ts>_<alias>/raw.csv` 并写入 header
/// - 已经在录制中则返回错误
#[tauri::command]
pub fn start_session(state: tauri::State<'_, AppState>) -> Result<String, String> {
    {
        let guard = state.session.lock().unwrap();
        if guard.is_some() {
            return Err("已有会话正在录制，请先停止".to_string());
        }
    }
    let aliases: Vec<String> = state
        .devices
        .lock()
        .unwrap()
        .values()
        .filter(|m| m.status == DeviceStatus::Connected)
        .map(|m| m.alias.clone())
        .collect();
    if aliases.is_empty() {
        return Err("没有已连接的设备，请先连接 LEFT/RIGHT".to_string());
    }
    let out_root = state.out_root.lock().unwrap().clone();
    let sess = SessionState::start(&out_root, &aliases)
        .map_err(|e| format!("创建会话失败：{}", e))?;
    let id = sess.session_id.clone();
    *state.session.lock().unwrap() = Some(sess);
    log::info!("session started: {}", id);
    Ok(id)
}

/// 暂停（保留 writer，可继续 resume）
#[tauri::command]
pub fn pause_session(state: tauri::State<'_, AppState>) -> Result<(), String> {
    let mut guard = state.session.lock().unwrap();
    match guard.as_mut() {
        Some(s) => {
            s.pause();
            Ok(())
        }
        None => Err("当前未在录制".to_string()),
    }
}

#[tauri::command]
pub fn resume_session(state: tauri::State<'_, AppState>) -> Result<(), String> {
    let mut guard = state.session.lock().unwrap();
    match guard.as_mut() {
        Some(s) => {
            s.resume();
            Ok(())
        }
        None => Err("当前未在录制".to_string()),
    }
}

/// 停止并 flush。返回每设备的行数与 csv 路径，前端用于展示「会话已保存」toast。
#[tauri::command]
pub fn stop_session(state: tauri::State<'_, AppState>) -> Result<SessionSummary, String> {
    let sess = state.session.lock().unwrap().take();
    match sess {
        Some(s) => s.stop().map_err(|e| format!("停止会话失败：{}", e)),
        None => Err("当前未在录制".to_string()),
    }
}

/// 暴露 out_root 给前端（SessionPanel / 一键流水线展示与拼路径用）
#[tauri::command]
pub fn get_out_root(state: tauri::State<'_, AppState>) -> String {
    state.out_root.lock().unwrap().to_string_lossy().to_string()
}

/// 用户在前端 dialog 选择新的会话输出根目录后调用。
///
/// 行为：
/// 1. 校验路径非空且可创建（不存在则 mkdir -p）
/// 2. 更新 `AppState.out_root`
/// 3. 持久化到 `<app_data>/app_config.json`（下次启动自动恢复）
///
/// 录制中是否允许切换：当前实现不阻拦——但下次 `start_session` 才生效，
/// 当前会话仍写入旧路径。前端可在「会话列表」UI 中给录制态时禁用按钮。
#[tauri::command]
pub fn set_out_root(
    path: String,
    app: tauri::AppHandle,
    state: tauri::State<'_, AppState>,
) -> Result<(), String> {
    let new_path = PathBuf::from(path.trim());
    if new_path.as_os_str().is_empty() {
        return Err("路径不能为空".to_string());
    }
    std::fs::create_dir_all(&new_path)
        .map_err(|e| format!("创建目录失败 {}: {}", new_path.display(), e))?;

    *state.out_root.lock().unwrap() = new_path.clone();

    // 持久化
    let app_data_dir = app
        .path()
        .app_data_dir()
        .map_err(|e| format!("无法获取 app_data_dir: {}", e))?;
    let cfg = AppConfig {
        out_root: Some(new_path.clone()),
    };
    if let Err(e) = cfg.save(&app_data_dir) {
        log::warn!("持久化 app_config 失败：{}", e);
        // 已经更新内存中的 out_root，持久化失败仅提示日志，不阻塞功能
    }
    log::info!("out_root updated to {}", new_path.display());
    Ok(())
}

/// 返回打包在发布包里的 build_dataset.py 默认绝对路径。
///
/// 通过 `app.path().resource_dir()` 解析 Tauri Resource，再拼 `build_dataset.py`。
/// 前端在「流水线设置」首次加载时调用，把它作为 `scriptPath` 的默认值；
/// 用户依然可以手动修改路径覆盖（例如指向 LingxiGlove/tools/ 下自己改过的版本）。
#[tauri::command]
pub fn get_default_script_path(app: tauri::AppHandle) -> Result<String, String> {
    let resource_dir = app
        .path()
        .resource_dir()
        .map_err(|e| format!("无法获取 resource_dir: {}", e))?;
    let script = resource_dir.join("resources").join("build_dataset.py");
    // dev 模式下 resource_dir 直接就是 src-tauri，路径形如 src-tauri/resources/build_dataset.py
    // release 模式下 Tauri 会把 resources/* 拷到 bundle 内对应位置，同样是 resource_dir/resources/build_dataset.py
    Ok(script.to_string_lossy().to_string())
}

// ---------------- 校准 Tab：3 个轻量 command ----------------
//
// 都是对 `serial_write_byte` 的语义化封装；结果异步通过 SerialTask 解析后
// 走 `calibration-event` 事件回到前端，本 command 只负责"发命令"。

/// 触发端侧个体校准流程（IMU 零偏 + Flex 量程）
///
/// 行为：先发 `r` 让端侧回到 RECOGNIZE 模式（端侧 [校准] 守卫要求），
/// 再发 `k` 触发 [`runCalibrationFlow`]。整个端侧流程约 21 秒，
/// 期间 firmware 阻塞 loop()，不响应其他串口命令。
#[tauri::command]
pub async fn start_calibration(
    alias: String,
    state: tauri::State<'_, AppState>,
) -> Result<(), String> {
    let tx = {
        let wm = state.writers.lock().unwrap();
        wm.get(&alias).cloned()
    };
    let tx = tx.ok_or_else(|| format!("设备 {} 未连接", alias))?;
    // 先 'r' 退出 capture / accuracy_test 等模式，再 'k' 进入校准
    tx.send(b"r\n".to_vec())
        .await
        .map_err(|e| format!("write 'r' failed: {}", e))?;
    // 留 50ms 让 firmware 处理完 'r' 的 DEBUG_LOG，避免命令拼接被忽略
    tokio::time::sleep(std::time::Duration::from_millis(50)).await;
    tx.send(b"k\n".to_vec())
        .await
        .map_err(|e| format!("write 'k' failed: {}", e))?;
    Ok(())
}

/// 请求端侧打印当前 NVS 校准内容（[CAL_INFO] 行）
///
/// 端侧响应通过 `calibration-event` Info 分支回到前端。
///
/// **实现细节**：发送 `info\n`（**而不是 `cal_show\n`**）。原因：
/// 老固件 `handleSerialCommand` 中 `case 'c'` 会**立即**把 `g_runMode` 切成
/// `MODE_CAPTURE`，再去看后续字符——这意味着 `cal_show` / `cal_clear` 会被
/// 误识别为「c 单字符 + al_show 残留」从而错误进入采集模式。
/// 而 firmware `info` 多字符命令（见 LingxiGlove_Main.ino 1433-1436 行）会
/// **同时**输出 `[CAL_INFO]` + `[CFG_INFO]` 两行，与 `cal_show` 在数据维度上
/// 等价，且首字符 'i' 是单字符命令分支不会污染模式状态。
#[tauri::command]
pub async fn read_calibration_state(
    alias: String,
    state: tauri::State<'_, AppState>,
) -> Result<(), String> {
    let tx = {
        let wm = state.writers.lock().unwrap();
        wm.get(&alias).cloned()
    };
    let tx = tx.ok_or_else(|| format!("设备 {} 未连接", alias))?;
    tx.send(b"info\n".to_vec())
        .await
        .map_err(|e| format!("write 'info' failed: {}", e))
}

/// 清除端侧 NVS 校准（恢复出厂默认）
///
/// 清除后端侧会回报一行 flags=0 的 [CAL_INFO]，前端据此刷新徽章状态。
///
/// **副作用兜底**：发送 `cal_clear\n` 时，老固件 `case 'c'` 会先把
/// `g_runMode` 切成 `MODE_CAPTURE`，紧接着才把后续 `al_clear` 拼出
/// 多字符命令——但模式已被污染。这里先发 `cal_clear`（必须保持原命令以
/// 落地清除动作），50ms 后追发一次 `r\n` 强制把固件拽回 `MODE_RECOGNIZE`。
#[tauri::command]
pub async fn clear_calibration(
    alias: String,
    state: tauri::State<'_, AppState>,
) -> Result<(), String> {
    let tx = {
        let wm = state.writers.lock().unwrap();
        wm.get(&alias).cloned()
    };
    let tx = tx.ok_or_else(|| format!("设备 {} 未连接", alias))?;
    tx.send(b"cal_clear\n".to_vec())
        .await
        .map_err(|e| format!("write 'cal_clear' failed: {}", e))?;
    // 兜底：'c' 单字符首会污染 g_runMode，50ms 后再发 'r' 复位识别模式
    tokio::time::sleep(std::time::Duration::from_millis(50)).await;
    tx.send(b"r\n".to_vec())
        .await
        .map_err(|e| format!("write 'r' (post cal_clear) failed: {}", e))
}

// ---------------- 设备配置 Tab：6 个轻量 command ----------------
//
// 全部是对 firmware `handleSerialCommand` 的语义化封装。端侧执行命令后
// 会写 NVS 并 **3 秒后自动重启**；重启后端侧重新打印 [CFG_INFO] 行，
// SerialTask 解析为 `calibration-event` Cfg 分支回到前端。
//
// 调用前可由前端弹出二次确认（"设备将自动重启"）。

/// 取出 alias 对应的串口 writer，未连接返回 Err
fn get_writer(
    alias: &str,
    state: &tauri::State<'_, AppState>,
) -> Result<mpsc::Sender<Vec<u8>>, String> {
    let wm = state.writers.lock().unwrap();
    wm.get(alias)
        .cloned()
        .ok_or_else(|| format!("设备 {} 未连接", alias))
}

/// 切换设备角色 (master / slave)。设备会写 NVS 并 3s 后重启。
#[tauri::command]
pub async fn set_device_role(
    alias: String,
    role: String,
    state: tauri::State<'_, AppState>,
) -> Result<(), String> {
    if role != "master" && role != "slave" {
        return Err(format!("非法角色 '{}', 必须是 master 或 slave", role));
    }
    let tx = get_writer(&alias, &state)?;
    let cmd = format!("role {}\n", role);
    tx.send(cmd.into_bytes())
        .await
        .map_err(|e| format!("write 'role' failed: {}", e))
}

/// 设置 ESP-NOW 对端 MAC（格式 AA:BB:CC:DD:EE:FF）。设备 3s 后重启。
#[tauri::command]
pub async fn set_peer_mac(
    alias: String,
    mac: String,
    state: tauri::State<'_, AppState>,
) -> Result<(), String> {
    let bytes = mac.as_bytes();
    if bytes.len() != 17 {
        return Err(format!("MAC 长度应为 17, 收到 '{}'", mac));
    }
    for (i, &c) in bytes.iter().enumerate() {
        if i % 3 == 2 {
            if c != b':' {
                return Err(format!("MAC 第 {} 位应为 ':' ('{}')", i + 1, mac));
            }
        } else if !c.is_ascii_hexdigit() {
            return Err(format!("MAC 含非 hex 字符 ('{}')", mac));
        }
    }
    let tx = get_writer(&alias, &state)?;
    let cmd = format!("peer {}\n", mac);
    tx.send(cmd.into_bytes())
        .await
        .map_err(|e| format!("write 'peer' failed: {}", e))
}

/// 设置 WiFi 凭据。设备 3s 后重启。
///
/// **注意**：firmware 端使用空格分隔 `wifi <ssid> <password>`，
/// 因此 SSID/密码不能包含空格。
#[tauri::command]
pub async fn set_wifi(
    alias: String,
    ssid: String,
    password: String,
    state: tauri::State<'_, AppState>,
) -> Result<(), String> {
    if ssid.is_empty() {
        return Err("SSID 不能为空".to_string());
    }
    if ssid.contains(' ') || ssid.contains('\n') || ssid.contains('\r') {
        return Err("SSID 不能包含空格或换行".to_string());
    }
    if password.contains('\n') || password.contains('\r') {
        return Err("密码不能包含换行".to_string());
    }
    let tx = get_writer(&alias, &state)?;
    let cmd = format!("wifi {} {}\n", ssid, password);
    tx.send(cmd.into_bytes())
        .await
        .map_err(|e| format!("write 'wifi' failed: {}", e))
}

/// 清除 WiFi NVS 凭据。设备 3s 后重启。
#[tauri::command]
pub async fn clear_device_wifi(
    alias: String,
    state: tauri::State<'_, AppState>,
) -> Result<(), String> {
    let tx = get_writer(&alias, &state)?;
    tx.send(b"wifi clear\n".to_vec())
        .await
        .map_err(|e| format!("write 'wifi clear' failed: {}", e))
}

/// 清除 NVS 中的角色 / 对端 MAC（恢复编译默认）。设备 3s 后重启。
#[tauri::command]
pub async fn clear_nvs_role(
    alias: String,
    state: tauri::State<'_, AppState>,
) -> Result<(), String> {
    let tx = get_writer(&alias, &state)?;
    tx.send(b"nvs clear\n".to_vec())
        .await
        .map_err(|e| format!("write 'nvs clear' failed: {}", e))
}

/// 请求端侧打印 [CFG_INFO] 行（角色 / MAC / WiFi SSID）。
///
/// firmware 收到 `info` 后会立刻打印一段人读信息 + 一行机读 [CFG_INFO]，
/// SerialTask 解析后通过 `calibration-event` Cfg 分支回到前端。
#[tauri::command]
pub async fn read_device_info(
    alias: String,
    state: tauri::State<'_, AppState>,
) -> Result<(), String> {
    let tx = get_writer(&alias, &state)?;
    tx.send(b"info\n".to_vec())
        .await
        .map_err(|e| format!("write 'info' failed: {}", e))
}
