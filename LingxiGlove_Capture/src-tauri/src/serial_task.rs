//! 单设备串口读取任务
//!
//! 每个连接的设备（left/right）启动一个 [`SerialTask`]，独立 tokio task：
//! 1. 打开 tokio_serial 异步串口
//! 2. 按行读取 ASCII（与现有 firmware 的 MODE_CAPTURE 协议一致）
//! 3. 解析为 [`Frame`]
//! 4. 通过 mpsc 发往 Aggregator
//!
//! **故障隔离**：单设备掉线只让本 task 退出，不影响另一设备和 UI。

use crate::types::{now_ms, DeviceAlias, Frame};
use anyhow::{anyhow, Result};
use futures_util::StreamExt;
use std::time::Duration;
use tauri::{AppHandle, Emitter};
use tokio::io::AsyncWriteExt;
use tokio::sync::mpsc;
use tokio_serial::SerialPortBuilderExt;
use tokio_util::codec::{FramedRead, LinesCodec};

/// 角色检测事件 payload（emit `serial-role` 事件）
#[derive(Clone, serde::Serialize)]
pub struct SerialRoleEvent {
    pub alias: String,
    /// "master" | "slave" | "unknown"
    pub role: String,
}

/// 单设备串口读取任务
pub struct SerialTask {
    pub alias: DeviceAlias,
    pub port: String,
    pub baud: u32,
    /// 收到的 Frame 全部 push 到这里
    pub tx: mpsc::Sender<Frame>,
    /// 取消信号（前端 disconnect / 应用退出）
    pub cancel: tokio::sync::watch::Receiver<bool>,
    /// 前端通过 `serial_write_byte` 命令发来的字节（c/r/i 等）
    pub write_rx: mpsc::Receiver<Vec<u8>>,
    /// 用于 emit `serial-role` 事件
    pub app_handle: AppHandle,
}

impl SerialTask {
    /// 启动循环。返回 Err 时前端可显示「连接失败」toast。
    pub async fn run(mut self) -> Result<()> {
        log::info!("[{}] open {} @ {}", self.alias, self.port, self.baud);

        // tokio-serial async open
        let port = tokio_serial::new(&self.port, self.baud)
            .timeout(std::time::Duration::from_millis(10))
            .open_native_async()
            .map_err(|e| anyhow!("open {} failed: {}", self.port, e))?;

        // 拆 read / write half：read half 给 LinesCodec，write half 给前端命令通道
        let (read_half, mut write_half) = tokio::io::split(port);
        let codec = LinesCodec::new_with_max_length(8192);
        let mut reader = FramedRead::new(read_half, codec);

        // 200ms 后主动写一次 'i'，触发板子打印 [配置] 角色横幅
        let probe_timer = tokio::time::sleep(Duration::from_millis(200));
        tokio::pin!(probe_timer);
        let mut probe_sent = false;

        // role 检测窗口：5s 内未匹配 MASTER/SLAVE 关键词就 emit unknown 兜底
        let role_timeout = tokio::time::sleep(Duration::from_secs(5));
        tokio::pin!(role_timeout);
        let mut role_resolved = false;

        loop {
            tokio::select! {
                // 取消信号
                _ = self.cancel.changed() => {
                    if *self.cancel.borrow() {
                        log::info!("[{}] cancelled", self.alias);
                        return Ok(());
                    }
                }
                // 200ms 主动探针
                _ = &mut probe_timer, if !probe_sent => {
                    probe_sent = true;
                    if let Err(e) = write_half.write_all(b"i\n").await {
                        log::warn!("[{}] probe write 'i' failed: {}", self.alias, e);
                    } else {
                        log::debug!("[{}] role probe 'i' sent", self.alias);
                    }
                }
                // 5s 后仍未识别 → emit unknown
                _ = &mut role_timeout, if !role_resolved => {
                    role_resolved = true;
                    let _ = self.app_handle.emit("serial-role", SerialRoleEvent {
                        alias: self.alias.clone(),
                        role: "unknown".to_string(),
                    });
                    log::info!("[{}] role probe timeout (5s), emit unknown", self.alias);
                }
                // 前端 invoke('serial_write_byte') 发来的字节
                Some(bytes) = self.write_rx.recv() => {
                    if let Err(e) = write_half.write_all(&bytes).await {
                        log::warn!("[{}] write {} bytes failed: {}", self.alias, bytes.len(), e);
                    }
                }
                // 串口下一行
                line = reader.next() => {
                    match line {
                        Some(Ok(text)) => {
                            // 角色识别：只在未确认时尝试
                            if !role_resolved {
                                if let Some(role) = detect_role(&text) {
                                    role_resolved = true;
                                    let _ = self.app_handle.emit("serial-role", SerialRoleEvent {
                                        alias: self.alias.clone(),
                                        role: role.to_string(),
                                    });
                                    log::info!("[{}] role detected: {} (line={:?})", self.alias, role, text);
                                }
                            }
                            // 校准协议优先级高于 CSV 解析：先 [CAL_INFO] 再 [CAL]
                            // ([CAL_INFO] 行内含 [CAL] 子串，必须先匹配长 marker)
                            if let Some(info) = crate::calibration::parse_cal_info(&text) {
                                let _ = self.app_handle.emit(
                                    "calibration-event",
                                    crate::calibration::CalEvent::Info {
                                        alias: self.alias.clone(),
                                        info,
                                    },
                                );
                                continue;
                            }
                            if let Some(progress) = crate::calibration::parse_cal_progress(&text) {
                                let _ = self.app_handle.emit(
                                    "calibration-event",
                                    crate::calibration::CalEvent::Progress {
                                        alias: self.alias.clone(),
                                        progress,
                                    },
                                );
                                continue;
                            }
                            // 设备配置 marker（角色 / MAC / WiFi SSID）
                            if let Some(cfg) = crate::calibration::parse_cfg_info(&text) {
                                let _ = self.app_handle.emit(
                                    "calibration-event",
                                    crate::calibration::CalEvent::Cfg {
                                        alias: self.alias.clone(),
                                        cfg,
                                    },
                                );
                                continue;
                            }
                            for frame in parse_line(&self.alias, &text) {
                                if self.tx.send(frame).await.is_err() {
                                    log::warn!("[{}] aggregator dropped, exit", self.alias);
                                    return Ok(());
                                }
                            }
                            // 解析失败的行（启动 banner / debug log）静默忽略
                        }
                        Some(Err(e)) => {
                            log::error!("[{}] read err: {}", self.alias, e);
                            return Err(anyhow!("read err: {}", e));
                        }
                        None => {
                            log::warn!("[{}] EOF", self.alias);
                            return Ok(());
                        }
                    }
                }
            }
        }
    }
}

/// 从一行串口输出里嗅探板子角色
///
/// 端侧（LingxiGlove_Main.ino）会从两条路径打印角色信息：
/// - **boot banner**（setup 内 NVS 加载日志）：
///     `[配置] NVS 角色: MASTER (覆盖编译期默认值)`
///     `[配置] NVS 无角色记录，使用编译期默认: SLAVE`
/// - **'i' 命令响应**（printDeviceInfo）：
///     `[配置] 当前角色: MASTER (NVS 已配置)`        ← 新版（含 [配置] 前缀）
///     `  角色:     MASTER (NVS 已配置)`             ← 旧版（无 [配置] 前缀，兼容）
///
/// 因此放宽 marker 条件：行内只要包含 `[配置]` 或 `角色:`（半角冒号）
/// 任一关键字 + `MASTER` / `SLAVE` 关键字，即视为角色行。
fn detect_role(text: &str) -> Option<&'static str> {
    let has_marker = text.contains("[配置]") || text.contains("角色:");
    if !has_marker {
        return None;
    }
    if text.contains("MASTER") {
        return Some("master");
    }
    if text.contains("SLAVE") {
        return Some("slave");
    }
    None
}

/// 解析一行 ASCII 数据为 0~2 个 Frame
///
/// **协议适配**
/// 1. **单手 15 列**（兼容现有 firmware 单手 MODE_CAPTURE）
///    `ts,ax,ay,az,gx,gy,gz,pitch,roll,flex0..4,label`
///    → 返回 1 个 Frame（dev_alias = SerialTask 配置的 alias）
///
/// 2. **双手联合 29 列**（firmware MASTER 角色 + ENABLE_ESPNOW_SYNC）
///    `ts, m_ax..m_roll, m_flex0..4, s_ax..s_roll, s_flex0..4, slave_age_ms, label`
///    → 返回 2 个 Frame：
///      - 左帧：dev_alias="left", values=[m_ax..m_flex4], bimanual_raw=Some(整行)
///      - 右帧：dev_alias="right", values=[s_ax..s_flex4], bimanual_raw=None
///
/// 3. **退化纯通道**（无 ts，所有 token 都是 f32）
///    → 返回 1 个 Frame，dev_ts_ms=0
///
/// 失败返回空 Vec（启动 banner / 注释行 / 不完整数据 全部静默丢弃）。
pub fn parse_line(alias: &DeviceAlias, raw: &str) -> Vec<Frame> {
    let line = raw.trim();
    if line.is_empty() || line.starts_with('#') || line.starts_with("//") {
        return Vec::new();
    }
    let toks: Vec<&str> = line.split(',').map(|s| s.trim()).collect();
    if toks.len() < 2 {
        return Vec::new();
    }

    // 第一个 token 当 dev_ts_ms (u32)；不是数字则当作纯通道行
    let (dev_ts_ms, value_toks) = match toks[0].parse::<u32>() {
        Ok(ts) => (ts, &toks[1..]),
        Err(_) => (0u32, &toks[..]),
    };

    let mut values: Vec<f32> = Vec::with_capacity(value_toks.len());
    for t in value_toks {
        match t.parse::<f32>() {
            Ok(v) => values.push(v),
            Err(_) => return Vec::new(),
        }
    }
    if values.is_empty() {
        return Vec::new();
    }

    let recv_ts = now_ms();

    // 判别：单手 14 个数值 (9 imu+pitch/roll + 5 flex + label) 或 28 个 (双手 + slave_age + label)
    // 双手联合：13 master + 13 slave + slave_age + label = 28
    if values.len() == 28 && dev_ts_ms != 0 {
        // -- bimanual 路径 --
        // values 顺序：m_ax..m_az, m_gx..m_gz, m_pitch, m_roll, m_flex0..4,
        //             s_ax..s_az, s_gx..s_gz, s_pitch, s_roll, s_flex0..4,
        //             slave_age_ms, label
        let master_vals: Vec<f32> = values[0..13].to_vec();
        let slave_vals: Vec<f32> = values[13..26].to_vec();
        let label_field = values[27] as i8;
        let left = Frame {
            dev_alias: "left".to_string(),
            recv_ts_ms: recv_ts,
            dev_ts_ms,
            label: label_field,
            values: master_vals,
            raw_line: line.to_string(),
            bimanual_raw: Some(line.to_string()),
        };
        let right = Frame {
            dev_alias: "right".to_string(),
            recv_ts_ms: recv_ts,
            dev_ts_ms,
            label: label_field,
            values: slave_vals,
            raw_line: String::new(),
            bimanual_raw: None,
        };
        return vec![left, right];
    }

    // -- 单手 / 退化路径 --
    vec![Frame {
        dev_alias: alias.clone(),
        recv_ts_ms: recv_ts,
        dev_ts_ms,
        label: -1, // 由 LabelBroadcaster 在 aggregator 注入
        values,
        raw_line: line.to_string(),
        bimanual_raw: None,
    }]
}

/// 单元测试：核心解析路径覆盖
#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parse_with_timestamp() {
        let frames = parse_line(&"left".to_string(), "12345,1024.0,512.0,800.0");
        assert_eq!(frames.len(), 1);
        let f = &frames[0];
        assert_eq!(f.dev_ts_ms, 12345);
        assert_eq!(f.values, vec![1024.0, 512.0, 800.0]);
        assert_eq!(f.dev_alias, "left");
        assert!(f.bimanual_raw.is_none());
    }

    #[test]
    fn parse_pure_channels() {
        // 第一个 token 是浮点 → 当通道
        let frames = parse_line(&"right".to_string(), "1.5,2.5,3.5");
        assert_eq!(frames.len(), 1);
        let f = &frames[0];
        assert_eq!(f.dev_ts_ms, 0);
        assert_eq!(f.values, vec![1.5, 2.5, 3.5]);
    }

    #[test]
    fn parse_garbage() {
        assert!(parse_line(&"left".to_string(), "").is_empty());
        assert!(parse_line(&"left".to_string(), "# comment").is_empty());
        assert!(parse_line(&"left".to_string(), "boot ok").is_empty());
        assert!(parse_line(&"left".to_string(), "12345,abc").is_empty());
    }

    #[test]
    fn detect_role_boot_banner() {
        // boot banner 路径：含 [配置]
        assert_eq!(
            detect_role("[1234ms] [配置] NVS 角色: MASTER (覆盖编译期默认值)"),
            Some("master")
        );
        assert_eq!(
            detect_role("[配置] NVS 无角色记录，使用编译期默认: SLAVE"),
            Some("slave")
        );
    }

    #[test]
    fn detect_role_i_response_new_format() {
        // 'i' 命令响应新版：含 [配置]
        assert_eq!(
            detect_role("[567ms] [配置] 当前角色: MASTER (NVS 已配置)"),
            Some("master")
        );
    }

    #[test]
    fn detect_role_i_response_legacy_format() {
        // 'i' 命令响应旧版：缩进 + "角色:"，不含 [配置]
        assert_eq!(
            detect_role("[567ms]   角色:     MASTER (NVS 已配置)"),
            Some("master")
        );
        assert_eq!(
            detect_role("  角色:     SLAVE (NVS 未配置对端)"),
            Some("slave")
        );
    }

    #[test]
    fn detect_role_unrelated_lines_ignored() {
        // 不含任何 marker
        assert!(detect_role("MASTER").is_none());
        assert!(detect_role("12345,1.0,2.0,...").is_none());
        // 含 marker 但无 MASTER/SLAVE 关键字
        assert!(detect_role("[配置] WiFi SSID: home").is_none());
        assert!(detect_role("  角色: 未知").is_none());
        // 注释 / 编译宏行不会被串口看到，但 defensive
        assert!(detect_role("// 0 = MASTER, 1 = SLAVE").is_none());
    }

    #[test]
    fn parse_bimanual_29cols() {
        // 29 列 = 1 ts + 13 master + 13 slave + slave_age + label
        // master 用 0.1 标识，slave 用 0.9 标识，便于断言拆分正确性
        let mut tokens = Vec::with_capacity(29);
        tokens.push("12345".to_string()); // ts
        for _ in 0..13 { tokens.push("0.1".to_string()); }
        for _ in 0..13 { tokens.push("0.9".to_string()); }
        tokens.push("80".to_string()); // slave_age_ms
        tokens.push("2".to_string());  // label
        let line = tokens.join(",");
        let frames = parse_line(&"ignored".to_string(), &line);
        assert_eq!(frames.len(), 2, "bimanual line should split into 2 frames");
        let l = &frames[0];
        let r = &frames[1];
        assert_eq!(l.dev_alias, "left");
        assert_eq!(r.dev_alias, "right");
        assert_eq!(l.values.len(), 13);
        assert_eq!(r.values.len(), 13);
        assert!(l.values.iter().all(|v| (*v - 0.1).abs() < 1e-4));
        assert!(r.values.iter().all(|v| (*v - 0.9).abs() < 1e-4));
        assert_eq!(l.label, 2);
        assert_eq!(r.label, 2);
        assert!(l.bimanual_raw.is_some(), "left frame must carry bimanual_raw");
        assert!(r.bimanual_raw.is_none(), "right frame must NOT carry bimanual_raw");
        assert_eq!(l.bimanual_raw.as_deref().unwrap(), &line);
    }
}
