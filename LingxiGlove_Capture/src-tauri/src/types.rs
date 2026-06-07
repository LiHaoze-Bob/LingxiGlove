//! 核心数据类型定义
//!
//! 所有跨模块、跨 Rust↔TS 边界的数据结构集中在这里：
//! - [`Frame`]：单条传感器数据（解析后），由 SerialTask 产出 → mpsc → Aggregator
//! - [`DeviceMeta`]：设备元信息（alias / port / baud / 状态）
//! - [`SerialPortInfo`]：串口枚举条目（前端下拉用）
//!
//! 录制会话状态见 [`crate::session::SessionState`]。

use serde::{Deserialize, Serialize};
use std::time::SystemTime;


/// 设备别名：MVP 阶段固定 "left" / "right"
pub type DeviceAlias = String;

/// 单条解析后的传感器帧
///
/// **字段语义**
/// - `dev_alias`：设备别名（"left" / "right"），由前端用户在连接时指定；
///                bimanual 模式下由后端拆帧时强制为 "left"（master）/ "right"（slave）
/// - `recv_ts_ms`：PC 接收时间戳（UNIX ms），是**全局时钟基准**
/// - `dev_ts_ms`：设备 millis() 自报，仅用于诊断，不参与对齐
/// - `label`：当前 label（由 PC 端 LabelBroadcaster 注入，未打标时 = -1）
/// - `values`：通道数据（单手 11 维：5 flex + 6 IMU；bimanual 拆帧时各 13 维）
/// - `raw_line`：原始 CSV 行（保留用于诊断和 raw_<alias>.csv 落盘）
/// - `bimanual_raw`：仅 bimanual 模式下 master 拆出的左帧才有值，
///                  Some(原 29 列整行) 表示该帧应写到 session 的 raw_bimanual.csv；
///                  右帧 / 单手帧均为 None
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Frame {
    pub dev_alias: DeviceAlias,
    pub recv_ts_ms: u64,
    pub dev_ts_ms: u32,
    pub label: i8,
    pub values: Vec<f32>,
    pub raw_line: String,
    #[serde(default)]
    pub bimanual_raw: Option<String>,
}

/// 设备元信息 + 当前连接状态
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct DeviceMeta {
    pub alias: DeviceAlias,
    pub port: String,
    pub baud: u32,
    pub status: DeviceStatus,
    /// 累计帧数（Day 1 用于状态栏帧率显示）
    pub frame_count: u64,
}

#[derive(Debug, Clone, Copy, Serialize, Deserialize, PartialEq, Eq)]
#[serde(rename_all = "lowercase")]
pub enum DeviceStatus {
    /// 未连接
    Idle,
    /// 串口已打开，正在读取
    Connected,
    /// 串口异常或断开
    Error,
}

/// 串口枚举条目（前端下拉用）
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct SerialPortInfo {
    /// 端口路径，如 "/dev/cu.usbserial-A50285BI"
    pub port_name: String,
    /// 描述（厂商名/产品名），方便用户分辨
    pub description: Option<String>,
    /// 类型：USB / Bluetooth / PCI / Unknown
    pub port_type: String,
}

/// 当前 UNIX 时间戳（ms），用于 Frame.recv_ts_ms
pub fn now_ms() -> u64 {
    SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map(|d| d.as_millis() as u64)
        .unwrap_or(0)
}
