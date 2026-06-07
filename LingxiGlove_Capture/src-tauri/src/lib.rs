//! LingxiCapture Tauri 后端入口
//!
//! 模块组织：
//! - `types`：跨边界数据结构
//! - `serial_task`：单设备串口读取（async）
//! - `aggregator`：多路 Frame 汇聚 + 推前端 + label 注入 + 会话写盘
//! - `commands`：Tauri command 接口
//! - `label`：全局当前 label（PC 端权威）
//! - `session`：录制会话 + raw.csv 落盘（与 build_dataset.py 兼容）

mod aggregator;
mod app_config;
mod calibration;
mod commands;
mod label;
mod pipeline;
mod secrets;
mod serial_task;
mod session;
mod types;

use aggregator::{spawn_aggregator, FpsMap, SharedSession};
use app_config::AppConfig;
use commands::AppState;
use label::LabelState;
use std::collections::HashMap;
use std::path::PathBuf;
use std::sync::{Arc, Mutex};
use tauri::Manager;
use tokio::sync::mpsc;

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    // 初始化日志
    let _ = env_logger::Builder::from_env(
        env_logger::Env::default().default_filter_or("info,lingxi_capture_lib=debug"),
    )
    .try_init();

    tauri::Builder::default()
        .plugin(tauri_plugin_opener::init())
        .plugin(tauri_plugin_dialog::init())
        .plugin(tauri_plugin_shell::init())
        .setup(|app| {
            let handle = app.handle().clone();
        
            // 1. 读取持久化配置（用户上次选过的 out_root）
            let app_data_dir = handle
                .path()
                .app_data_dir()
                .unwrap_or_else(|_| PathBuf::from("."));
            let cfg = AppConfig::load(&app_data_dir);
        
            // 2. 决定 out_root：优先用户自定义 → 其次 <app_data>/output/capture → 兑底 cwd
            let out_root = cfg.out_root.clone().unwrap_or_else(|| {
                match handle.path().app_data_dir() {
                    Ok(p) => p.join("output").join("capture"),
                    Err(_) => std::env::current_dir()
                        .unwrap_or_else(|_| PathBuf::from("."))
                        .join("output")
                        .join("capture"),
                }
            });
            if let Err(e) = std::fs::create_dir_all(&out_root) {
                log::warn!("无法创建输出目录 {}: {}", out_root.display(), e);
            }
            log::info!("output root: {}", out_root.display());
        
            // 全局通道：所有 SerialTask → Aggregator（容量 1024 缓冲短时洪峰）
            let (frame_tx, frame_rx) = mpsc::channel(1024);
            let fps_map: FpsMap = Arc::new(Mutex::new(HashMap::new()));
            let label_state = Arc::new(LabelState::new());
            let session: SharedSession = Arc::new(Mutex::new(None));
        
            handle.manage(AppState {
                frame_tx,
                cancellers: Arc::new(Mutex::new(HashMap::new())),
                writers: Arc::new(Mutex::new(HashMap::new())),
                devices: Arc::new(Mutex::new(HashMap::new())),
                fps_map: fps_map.clone(),
                label_state: label_state.clone(),
                session: session.clone(),
                out_root: Arc::new(Mutex::new(out_root)),
            });

            spawn_aggregator(frame_rx, handle.clone(), fps_map, label_state, session);
            log::info!("LingxiCapture started");
            Ok(())
        })
        .invoke_handler(tauri::generate_handler![
            commands::list_ports,
            commands::connect_device,
            commands::disconnect_device,
            commands::serial_write_byte,
            commands::get_devices,
            commands::get_fps,
            commands::set_label,
            commands::get_label,
            commands::get_session_info,
            commands::start_session,
            commands::pause_session,
            commands::resume_session,
            commands::stop_session,
            commands::get_out_root,
            commands::set_out_root,
            commands::get_default_script_path,
            commands::start_calibration,
            commands::read_calibration_state,
            commands::clear_calibration,
            commands::set_device_role,
            commands::set_peer_mac,
            commands::set_wifi,
            commands::clear_device_wifi,
            commands::clear_nvs_role,
            commands::read_device_info,
            pipeline::list_sessions,
            pipeline::delete_session,
            pipeline::run_build_dataset,
            pipeline::upload_to_ei,
            pipeline::run_pipeline,
            pipeline::set_ei_key,
            pipeline::has_ei_key,
            pipeline::delete_ei_key,
        ])
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}
