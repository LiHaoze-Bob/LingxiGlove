//! 录制会话管理 + raw.csv 落盘
//!
//! ## 兼容性铁律（与 LingxiGlove/tools/build_dataset.py 0 修改对接）
//!
//! 1. **目录结构**：`<out_root>/session_<YYYYMMDD_HHMMSS>_<alias>/raw.csv`
//!    - build_dataset.py 用 `glob("session_*/raw.csv")` 枚举，文件名必须严格 `raw.csv`
//!    - 同一会话双手分两个目录（按 alias 后缀区分），单手切窗各自独立
//!
//! 2. **CSV header**（与端侧 firmware `printCsvHeader()` 字节级一致）：
//!    `timestamp_ms,ax,ay,az,gx,gy,gz,pitch,roll,flex0,flex1,flex2,flex3,flex4,label`
//!    - 共 15 列，最后一列是 label
//!
//! 3. **数据行写入**：直接复用 [`Frame::raw_line`]（端侧已经按上述 15 列格式化好），
//!    仅把最后一列覆写为 GUI 当前 label。这样浮点精度、整型/浮点混合都与
//!    端侧自身落盘 100% 一致，规避任何格式转换偏差。
//!
//! ## 文件 I/O 策略
//! - 每 alias 一个 [`std::io::BufWriter`]，避免每帧 syscall
//! - 每 50 帧（约 2.5s @ 20Hz）flush 一次，崩溃时最多丢 2.5s 数据
//! - stop_session 时显式 flush + drop

use crate::types::{DeviceAlias, Frame};
use anyhow::{anyhow, Context, Result};
use chrono::Local;
use serde::Serialize;
use std::collections::HashMap;
use std::fs::File;
use std::io::{BufWriter, Write};
use std::path::{Path, PathBuf};

/// 端侧 firmware printCsvHeader() 的固定 header（单手 15 列），与 ENABLE_FLEX_SENSORS=1 路径对齐
pub const CSV_HEADER: &str =
    "timestamp_ms,ax,ay,az,gx,gy,gz,pitch,roll,flex0,flex1,flex2,flex3,flex4,label";

/// 端侧 firmware printBimanualCsvHeader() 的固定 header（双手 29 列），
/// MASTER + ENABLE_ESPNOW_SYNC + capture 模式输出格式
pub const CSV_BIMANUAL_HEADER: &str = "timestamp_ms,\
m_ax,m_ay,m_az,m_gx,m_gy,m_gz,m_pitch,m_roll,m_flex0,m_flex1,m_flex2,m_flex3,m_flex4,\
s_ax,s_ay,s_az,s_gx,s_gy,s_gz,s_pitch,s_roll,s_flex0,s_flex1,s_flex2,s_flex3,s_flex4,\
slave_age_ms,label";

/// bimanual 子目录别名（在 session 根下挂 `session_<ts>_bimanual/raw.csv`）
const BIMANUAL_ALIAS: &str = "bimanual";

/// 每多少行 flush 一次（与 capture_serial.py 的 50 行 flush 策略一致）
const FLUSH_INTERVAL: u64 = 50;

/// 单设备 raw.csv writer
pub struct SessionWriter {
    #[allow(dead_code)] // 诊断用：日志可查；getter 仅暴露 csv_path
    pub alias: DeviceAlias,
    pub csv_path: PathBuf,
    writer: BufWriter<File>,
    rows_written: u64,
    /// 已经被打过有效 label（>=0）的行数（仅用于结束摘要）
    labeled_rows: u64,
}

impl SessionWriter {
    /// 在 `session_dir` 内打开/创建 `raw.csv`，写入指定 header。
    pub fn create_with_header(session_dir: &Path, alias: &str, header: &str) -> Result<Self> {
        let csv_path = session_dir.join("raw.csv");
        let f = File::create(&csv_path)
            .with_context(|| format!("create {} failed", csv_path.display()))?;
        let mut writer = BufWriter::new(f);
        writeln!(writer, "{}", header)?;
        writer.flush()?;
        log::info!(
            "[session/{}] writer ready -> {}",
            alias,
            csv_path.display()
        );
        Ok(Self {
            alias: alias.to_string(),
            csv_path,
            writer,
            rows_written: 0,
            labeled_rows: 0,
        })
    }

    /// 单手 15 列 header 的便捷构造（保留旧 API）
    pub fn create(session_dir: &Path, alias: &str) -> Result<Self> {
        Self::create_with_header(session_dir, alias, CSV_HEADER)
    }

    /// 写一帧数据
    ///
    /// `raw_line` 来自端侧 CSV 行（已 trim），把最后一列覆写为 GUI 当前 `label`。
    /// 端侧没有按该 schema 输出（如 banner、纯通道行），调用方应在 aggregator 层
    /// 过滤；此处简单兜底，不抛错只记日志。
    pub fn write_frame(&mut self, raw_line: &str, label: i8) -> Result<()> {
        let final_line = substitute_last_label(raw_line, label);
        writeln!(self.writer, "{}", final_line)?;
        self.rows_written += 1;
        if label >= 0 {
            self.labeled_rows += 1;
        }
        if self.rows_written % FLUSH_INTERVAL == 0 {
            self.writer.flush()?;
        }
        Ok(())
    }

    pub fn flush(&mut self) -> Result<()> {
        self.writer.flush().map_err(Into::into)
    }

    pub fn rows_written(&self) -> u64 {
        self.rows_written
    }

    pub fn labeled_rows(&self) -> u64 {
        self.labeled_rows
    }
}

/// 把 CSV 行的最后一列替换为新 label。若原行无逗号则原样返回（兜底）。
pub fn substitute_last_label(raw: &str, label: i8) -> String {
    match raw.rfind(',') {
        Some(idx) => {
            let mut s = String::with_capacity(raw.len() + 4);
            s.push_str(&raw[..=idx]);
            s.push_str(&label.to_string());
            s
        }
        None => raw.to_string(),
    }
}

/// 整个录制会话（可包含多个设备的 SessionWriter）
#[allow(dead_code)] // session_dir_root / device_dirs 留作日后调试与"重打标"功能用
pub struct SessionState {
    pub session_id: String,
    pub session_dir_root: PathBuf,
    /// 时间戳前缀（如 "20260527_103245"），bimanual 子目录懒建时复用
    pub ts_prefix: String,
    /// alias -> 子目录 path
    pub device_dirs: HashMap<DeviceAlias, PathBuf>,
    /// alias -> writer（仅单手帧路径写入）
    pub writers: HashMap<DeviceAlias, SessionWriter>,
    /// bimanual writer：收到首个 bimanual_raw 帧时懒创建 `session_<ts>_bimanual/raw.csv`
    pub bimanual_writer: Option<SessionWriter>,
    pub started_at: chrono::DateTime<Local>,
    /// pause / resume 状态：暂停时 aggregator 仍把 frame 推前端但不写盘
    pub paused: bool,
}

impl SessionState {
    /// 新建一个会话目录，给 `aliases` 中每个别名建一个 `session_<ts>_<alias>/raw.csv`
    pub fn start(out_root: &Path, aliases: &[DeviceAlias]) -> Result<Self> {
        if aliases.is_empty() {
            return Err(anyhow!("无设备连接，无法开始会话"));
        }
        std::fs::create_dir_all(out_root)
            .with_context(|| format!("mkdir {} failed", out_root.display()))?;

        let started_at = Local::now();
        let ts = started_at.format("%Y%m%d_%H%M%S").to_string();
        let session_id = format!("session_{}", ts);

        let mut device_dirs = HashMap::new();
        let mut writers = HashMap::new();
        for alias in aliases {
            let sub = out_root.join(format!("session_{}_{}", ts, alias));
            std::fs::create_dir_all(&sub)
                .with_context(|| format!("mkdir {} failed", sub.display()))?;
            let w = SessionWriter::create(&sub, alias)?;
            device_dirs.insert(alias.clone(), sub);
            writers.insert(alias.clone(), w);
        }

        Ok(Self {
            session_id,
            session_dir_root: out_root.to_path_buf(),
            ts_prefix: ts,
            device_dirs,
            writers,
            bimanual_writer: None,
            started_at,
            paused: false,
        })
    }

    /// 懒创建 bimanual writer（首个 bimanual_raw 帧到达时调用）
    fn ensure_bimanual_writer(&mut self) -> Result<&mut SessionWriter> {
        if self.bimanual_writer.is_none() {
            let sub = self
                .session_dir_root
                .join(format!("session_{}_{}", self.ts_prefix, BIMANUAL_ALIAS));
            std::fs::create_dir_all(&sub)
                .with_context(|| format!("mkdir {} failed", sub.display()))?;
            let w = SessionWriter::create_with_header(&sub, BIMANUAL_ALIAS, CSV_BIMANUAL_HEADER)?;
            self.device_dirs.insert(BIMANUAL_ALIAS.to_string(), sub);
            self.bimanual_writer = Some(w);
            log::info!("[session] bimanual writer 懒创建完成");
        }
        Ok(self.bimanual_writer.as_mut().unwrap())
    }

    /// 写一帧；非录制中（paused）静默丢弃。
    /// 路由策略：
    ///   - frame.bimanual_raw = Some(原 29 列) → 写 bimanual writer（懒建）
    ///   - frame.raw_line 非空 → 写 alias writer（单手 / bimanual 拆出的左帧已被前者拦截）
    ///   - frame.raw_line 为空 → 静默忽略（bimanual 拆出的右帧）
    pub fn on_frame_obj(&mut self, frame: &Frame) -> Result<()> {
        if self.paused {
            return Ok(());
        }
        if let Some(raw29) = frame.bimanual_raw.as_deref() {
            let bw = self.ensure_bimanual_writer()?;
            bw.write_frame(raw29, frame.label)?;
            return Ok(());
        }
        if frame.raw_line.is_empty() {
            return Ok(());
        }
        if let Some(w) = self.writers.get_mut(&frame.dev_alias) {
            w.write_frame(&frame.raw_line, frame.label)?;
        }
        Ok(())
    }

    /// 兼容旧接口：单手帧的 (alias, raw_line, label) 写盘
    /// 仅供测试和遗留路径使用；新代码请用 [`on_frame_obj`]。
    #[allow(dead_code)]
    pub fn on_frame(&mut self, alias: &str, raw_line: &str, label: i8) -> Result<()> {
        if self.paused {
            return Ok(());
        }
        if let Some(w) = self.writers.get_mut(alias) {
            w.write_frame(raw_line, label)?;
        }
        Ok(())
    }

    /// 暂停（不丢弃 writer，可继续 resume）
    pub fn pause(&mut self) {
        self.paused = true;
    }

    pub fn resume(&mut self) {
        self.paused = false;
    }

    /// 结束并 flush 所有 writer，返回各设备的行数摘要
    pub fn stop(mut self) -> Result<SessionSummary> {
        let mut per_device = Vec::with_capacity(self.writers.len() + 1);
        for (alias, mut w) in self.writers.drain() {
            let _ = w.flush();
            per_device.push(DeviceSessionSummary {
                alias: alias.clone(),
                csv_path: w.csv_path.to_string_lossy().into_owned(),
                rows: w.rows_written(),
                labeled_rows: w.labeled_rows(),
            });
        }
        // bimanual writer（若有）也 flush 并加入 summary
        if let Some(mut bw) = self.bimanual_writer.take() {
            let _ = bw.flush();
            per_device.push(DeviceSessionSummary {
                alias: BIMANUAL_ALIAS.to_string(),
                csv_path: bw.csv_path.to_string_lossy().into_owned(),
                rows: bw.rows_written(),
                labeled_rows: bw.labeled_rows(),
            });
        }
        per_device.sort_by(|a, b| a.alias.cmp(&b.alias));
        Ok(SessionSummary {
            session_id: self.session_id,
            started_at_ms: self.started_at.timestamp_millis() as u64,
            duration_ms: (Local::now() - self.started_at)
                .num_milliseconds()
                .max(0) as u64,
            per_device,
        })
    }
}

/// 会话结束时返回给前端展示
#[derive(Debug, Clone, Serialize)]
pub struct DeviceSessionSummary {
    pub alias: String,
    pub csv_path: String,
    pub rows: u64,
    pub labeled_rows: u64,
}

#[derive(Debug, Clone, Serialize)]
pub struct SessionSummary {
    pub session_id: String,
    pub started_at_ms: u64,
    pub duration_ms: u64,
    pub per_device: Vec<DeviceSessionSummary>,
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn substitute_replaces_only_last_field() {
        let s = "12345,0.0120,0.0034,0.99,500,512,420,300,250,-1";
        assert_eq!(
            substitute_last_label(s, 2),
            "12345,0.0120,0.0034,0.99,500,512,420,300,250,2"
        );
    }

    #[test]
    fn substitute_handles_no_comma() {
        assert_eq!(substitute_last_label("foo", 5), "foo");
    }

    #[test]
    fn substitute_keeps_negative_label() {
        let s = "1,2,3";
        assert_eq!(substitute_last_label(s, -1), "1,2,-1");
    }

    #[test]
    fn writer_writes_header_and_rows() -> Result<()> {
        let dir = tempfile::tempdir()?;
        let mut w = SessionWriter::create(dir.path(), "left")?;
        // 模拟端侧 raw line（最后字段已是 -1）
        w.write_frame(
            "100,0.01,0.02,0.99,1.0,2.0,3.0,10.0,5.0,400,500,600,420,300,-1",
            2,
        )?;
        w.write_frame(
            "150,0.01,0.02,0.99,1.0,2.0,3.0,10.0,5.0,401,501,601,421,301,-1",
            2,
        )?;
        w.flush()?;

        let content = std::fs::read_to_string(dir.path().join("raw.csv"))?;
        let lines: Vec<&str> = content.lines().collect();
        assert_eq!(lines.len(), 3);
        assert_eq!(lines[0], CSV_HEADER);
        assert!(lines[1].ends_with(",2"));
        assert!(lines[2].ends_with(",2"));
        assert_eq!(w.rows_written(), 2);
        assert_eq!(w.labeled_rows(), 2);
        Ok(())
    }

    #[test]
    fn session_lifecycle() -> Result<()> {
        let dir = tempfile::tempdir()?;
        let mut s = SessionState::start(
            dir.path(),
            &["left".to_string(), "right".to_string()],
        )?;
        s.on_frame("left", "1,0,0,0,0,0,0,0,0,1,2,3,4,5,-1", 0)?;
        s.on_frame("right", "1,0,0,0,0,0,0,0,0,1,2,3,4,5,-1", 0)?;
        s.pause();
        s.on_frame("left", "2,0,0,0,0,0,0,0,0,9,9,9,9,9,-1", 1)?;
        s.resume();
        s.on_frame("left", "3,0,0,0,0,0,0,0,0,1,2,3,4,5,-1", 1)?;
        // 不在 aliases 列表中 → 静默丢弃，不报错
        s.on_frame("middle", "4,0,0,0,0,0,0,0,0,1,2,3,4,5,-1", 1)?;

        let summary = s.stop()?;
        let left = summary.per_device.iter().find(|d| d.alias == "left").unwrap();
        let right = summary.per_device.iter().find(|d| d.alias == "right").unwrap();
        assert_eq!(left.rows, 2);
        assert_eq!(right.rows, 1);
        Ok(())
    }

    #[test]
    fn bimanual_writer_lazy_creates_and_writes() -> Result<()> {
        let dir = tempfile::tempdir()?;
        let mut s = SessionState::start(dir.path(), &["left".to_string()])?;

        // 模拟一行 bimanual 29 列 raw（值任意，关键是逗号数 = 28）
        let bi_raw = "12345,0.1,0.1,0.1,0.1,0.1,0.1,0.1,0.1,1,1,1,1,1,\
0.9,0.9,0.9,0.9,0.9,0.9,0.9,0.9,9,9,9,9,9,80,-1";
        let left_frame = Frame {
            dev_alias: "left".to_string(),
            recv_ts_ms: 0,
            dev_ts_ms: 12345,
            label: 2,
            values: vec![0.1; 13],
            raw_line: bi_raw.to_string(),
            bimanual_raw: Some(bi_raw.to_string()),
        };
        let right_frame = Frame {
            dev_alias: "right".to_string(),
            recv_ts_ms: 0,
            dev_ts_ms: 12345,
            label: 2,
            values: vec![0.9; 13],
            raw_line: String::new(),
            bimanual_raw: None,
        };

        // 单手 left frame 仍正常写
        let single_left = Frame {
            dev_alias: "left".to_string(),
            recv_ts_ms: 0,
            dev_ts_ms: 100,
            label: 0,
            values: vec![0.0; 14],
            raw_line: "100,0.01,0.02,0.99,1.0,2.0,3.0,10.0,5.0,400,500,600,420,300,-1".to_string(),
            bimanual_raw: None,
        };

        s.on_frame_obj(&single_left)?;
        s.on_frame_obj(&left_frame)?;
        s.on_frame_obj(&right_frame)?; // 应被静默忽略（raw_line 空）
        s.on_frame_obj(&left_frame)?;  // 第 2 行 bimanual

        let summary = s.stop()?;
        // left 单手目录有 1 行；bimanual 目录有 2 行
        let left = summary.per_device.iter().find(|d| d.alias == "left").unwrap();
        let bi = summary.per_device.iter().find(|d| d.alias == "bimanual").unwrap();
        assert_eq!(left.rows, 1, "single-hand left should have 1 row");
        assert_eq!(bi.rows, 2, "bimanual should have 2 rows");
        // bimanual raw.csv 第一行是 29 列 header
        let bi_content = std::fs::read_to_string(&bi.csv_path)?;
        let first_line = bi_content.lines().next().unwrap();
        assert_eq!(first_line, CSV_BIMANUAL_HEADER);
        Ok(())
    }
}
