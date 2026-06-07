//! 校准协议解析（与 firmware 端 LingxiGlove_Main.ino + calibration.cpp 配套）
//!
//! 端侧打印两类机器可读行：
//!
//! 1. 进度 marker（`runCalibrationFlow` 内部多次发出）：
//!    ```text
//!    [CAL] stage=overall phase=start
//!    [CAL] stage=imu phase=countdown remain=3
//!    [CAL] stage=imu phase=sampling
//!    [CAL] stage=imu phase=done ok=1
//!    [CAL] stage=flex_min phase=countdown remain=3
//!    ...
//!    [CAL] stage=flex_max phase=done ok=0 reason=range_too_small
//!    [CAL] stage=save phase=done ok=1
//!    [CAL] stage=overall phase=done ok=1 flags=3
//!    ```
//!
//! 2. NVS 校准内容（`PrintCalibrationMachineReadable`，`cal_show` / 校准结束 / `info` 都会打印）：
//!    ```text
//!    [CAL_INFO] flags=3 ax=0.0123 ay=-0.0050 az=0.0110 gx=0.20 gy=-0.10 gz=0.05 \
//!               fmin=1100,1090,1110,1080,1095 fmax=2780,2810,2790,2820,2800
//!    ```
//!
//! 解析器对每一行做"先看 marker → 再 split key=val → 容错缺字段"的纯字符串处理，
//! 不引入 regex 依赖，便于 ARM/x86 二进制体积控制。

use serde::Serialize;

// ---------------------- 进度事件 ----------------------

#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum CalStage {
    Overall,
    Imu,
    FlexMin,
    FlexMax,
    Save,
}

#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
#[serde(rename_all = "snake_case")]
pub enum CalPhase {
    /// `phase=start`（仅 overall 阶段会发，作为 wizard 的"已收到板上确认"信号）
    Start,
    /// `phase=countdown remain=N`（N=3..1）
    Countdown,
    /// `phase=sampling`
    Sampling,
    /// `phase=done ok=0|1 [reason=...] [flags=...]`
    Done,
}

#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct CalProgress {
    pub stage: CalStage,
    pub phase: CalPhase,
    /// countdown 阶段的剩余秒数；其它阶段为 None
    pub remain: Option<u8>,
    /// done 阶段的成功标志；其它阶段为 None
    pub ok: Option<bool>,
    /// done 阶段的失败原因；只在 ok=false 出现
    pub reason: Option<String>,
    /// done 阶段的 flags 位图（仅 stage=overall 时端侧会带；其它阶段为 None）
    pub flags: Option<u16>,
}

// ---------------------- NVS 校准内容 ----------------------

#[derive(Debug, Clone, PartialEq, Serialize)]
pub struct CalInfo {
    pub flags: u16,
    pub accel_bias: [f32; 3],
    pub gyro_bias: [f32; 3],
    /// flex_min[0..5]；当 firmware 关闭 ENABLE_FLEX_SENSORS 时为全 0
    pub flex_min: [u16; 5],
    pub flex_max: [u16; 5],
}

// ---------------------- 联合事件（emit 给前端）----------------------

#[derive(Debug, Clone, Serialize)]
#[serde(tag = "kind", rename_all = "snake_case")]
pub enum CalEvent {
    Progress {
        alias: String,
        #[serde(flatten)]
        progress: CalProgress,
    },
    Info {
        alias: String,
        #[serde(flatten)]
        info: CalInfo,
    },
    /// 设备配置（角色 / MAC / WiFi SSID）—— 由 `[CFG_INFO]` 行触发
    Cfg {
        alias: String,
        #[serde(flatten)]
        cfg: CfgInfo,
    },
}

// ---------------------- 设备配置（NVS 持久化的角色/对端MAC/WiFi）----------------------

/// `[CFG_INFO] role=master|slave self_mac=AA:.. peer_mac=AA:..|none ssid=xxx \
///  wifi=connected|disconnected ip=192.168.x.x|none rssi=N|none \
///  mode=recognize|capture|finger_spelling|accuracy_test` 单行解析结果。
///
/// 由 firmware `printDeviceInfo` 末尾发出（`info` / `i` 命令均会触发）。
#[derive(Debug, Clone, PartialEq, Eq, Serialize)]
pub struct CfgInfo {
    /// "master" / "slave"（与 firmware g_runtime_role 一致）
    pub role: String,
    /// 本机 WiFi MAC，如 "AA:BB:CC:DD:EE:FF"
    pub self_mac: String,
    /// ESP-NOW 对端 MAC；未配置时为 None（firmware 端打印 "peer_mac=none"）
    pub peer_mac: Option<String>,
    /// 当前 WiFi SSID
    pub ssid: String,
    /// 当前 WiFi 密码（v4 新增；空/未配置 = None）。
    /// 安全提示：固件以明文输出此字段；上位机串口日志会留痕，请勿提交进 git。
    /// 老固件无此字段时 = None，UI 不反填。
    pub wifi_pwd: Option<String>,
    /// WiFi 是否已连接（WL_CONNECTED）。老固件不输出此字段时回退为 false。
    pub wifi_connected: bool,
    /// WiFi 已连接时的 IP（如 "192.168.1.100"）；未连接 / 老固件 = None
    pub ip: Option<String>,
    /// WiFi 已连接时的 RSSI（dBm，可为负）；未连接 / 老固件 = None
    pub rssi: Option<i32>,
    /// 当前运行模式（v3 新增）。可能值：
    /// "recognize" / "capture" / "finger_spelling" / "accuracy_test" / "unknown"。
    /// 老固件不输出此字段时为 None。校准 Tab 据此显示模式徽章并提示用户
    /// 「当前固件不在识别模式」。
    pub mode: Option<String>,
}

// ---------------------- 解析器 ----------------------

/// 提取一行里 `key=val` 形式的字段（val 不含空格，遇空格即截断）
fn extract_kv<'a>(text: &'a str, key: &str) -> Option<&'a str> {
    // 找到 " key=" 或行首 "key="，避免被 "fmax=..." 中的 "ax=" 误匹配
    let pat_mid = format!(" {}=", key);
    let pat_head = format!("{}=", key);
    let idx = if let Some(i) = text.find(&pat_mid) {
        i + pat_mid.len()
    } else if text.starts_with(&pat_head) {
        pat_head.len()
    } else {
        return None;
    };
    let rest = &text[idx..];
    // val 截断到下一个空白或行尾
    let end = rest.find(|c: char| c.is_whitespace()).unwrap_or(rest.len());
    Some(&rest[..end])
}

fn parse_stage(s: &str) -> Option<CalStage> {
    match s {
        "overall" => Some(CalStage::Overall),
        "imu" => Some(CalStage::Imu),
        "flex_min" => Some(CalStage::FlexMin),
        "flex_max" => Some(CalStage::FlexMax),
        "save" => Some(CalStage::Save),
        _ => None,
    }
}

fn parse_phase(s: &str) -> Option<CalPhase> {
    match s {
        "start" => Some(CalPhase::Start),
        "countdown" => Some(CalPhase::Countdown),
        "sampling" => Some(CalPhase::Sampling),
        "done" => Some(CalPhase::Done),
        _ => None,
    }
}

/// 解析 `[CAL] stage=... phase=... ...` 进度行
///
/// 行内 firmware 可能附加 `[<millis>ms] ` 时间戳前缀（DEBUG_LOG 自动加），
/// 解析器对前缀完全无感（用 `[CAL]` 子串作锚）。
pub fn parse_cal_progress(line: &str) -> Option<CalProgress> {
    // 找到 `[CAL]`（注意排除 `[CAL_INFO]`）
    let marker_idx = line.find("[CAL]")?;
    // 防御：紧挨着字符不能是字母/下划线（避免误匹配 `[CAL_INFO]` 的子串 `[CAL]`，
    // 实际 `[CAL_INFO]` 包含 `_INFO`，rfind 永远不会让 `[CAL]` 命中其前缀，
    // 但显式校验更稳健）
    let after_marker = &line[marker_idx + "[CAL]".len()..];
    if after_marker.starts_with('_') {
        return None;
    }

    let stage_s = extract_kv(after_marker, "stage")?;
    let phase_s = extract_kv(after_marker, "phase")?;
    let stage = parse_stage(stage_s)?;
    let phase = parse_phase(phase_s)?;

    let remain = if phase == CalPhase::Countdown {
        extract_kv(after_marker, "remain").and_then(|s| s.parse::<u8>().ok())
    } else {
        None
    };

    let (ok, reason, flags) = if phase == CalPhase::Done {
        let ok = extract_kv(after_marker, "ok").and_then(|s| match s {
            "1" => Some(true),
            "0" => Some(false),
            _ => None,
        });
        let reason = extract_kv(after_marker, "reason").map(|s| s.to_string());
        let flags = extract_kv(after_marker, "flags").and_then(|s| s.parse::<u16>().ok());
        (ok, reason, flags)
    } else {
        (None, None, None)
    };

    Some(CalProgress {
        stage,
        phase,
        remain,
        ok,
        reason,
        flags,
    })
}

/// 解析逗号分隔的 5 元素 u16 数组（如 `1100,1090,1110,1080,1095`）
fn parse_u16x5(s: &str) -> Option<[u16; 5]> {
    let mut out = [0u16; 5];
    let mut iter = s.split(',');
    for slot in out.iter_mut() {
        let tok = iter.next()?;
        *slot = tok.parse::<u16>().ok()?;
    }
    if iter.next().is_some() {
        // 多于 5 个，视为格式错误
        return None;
    }
    Some(out)
}

/// 解析 `[CAL_INFO] flags=... ax=... ... fmin=... fmax=...` 单行
pub fn parse_cal_info(line: &str) -> Option<CalInfo> {
    let marker_idx = line.find("[CAL_INFO]")?;
    let after = &line[marker_idx + "[CAL_INFO]".len()..];

    let flags = extract_kv(after, "flags").and_then(|s| s.parse::<u16>().ok())?;
    let ax = extract_kv(after, "ax").and_then(|s| s.parse::<f32>().ok())?;
    let ay = extract_kv(after, "ay").and_then(|s| s.parse::<f32>().ok())?;
    let az = extract_kv(after, "az").and_then(|s| s.parse::<f32>().ok())?;
    let gx = extract_kv(after, "gx").and_then(|s| s.parse::<f32>().ok())?;
    let gy = extract_kv(after, "gy").and_then(|s| s.parse::<f32>().ok())?;
    let gz = extract_kv(after, "gz").and_then(|s| s.parse::<f32>().ok())?;

    // ENABLE_FLEX_SENSORS=0 时 firmware 不输出 fmin/fmax；视为全 0
    let flex_min = extract_kv(after, "fmin")
        .and_then(parse_u16x5)
        .unwrap_or([0u16; 5]);
    let flex_max = extract_kv(after, "fmax")
        .and_then(parse_u16x5)
        .unwrap_or([0u16; 5]);

    Some(CalInfo {
        flags,
        accel_bias: [ax, ay, az],
        gyro_bias: [gx, gy, gz],
        flex_min,
        flex_max,
    })
}

/// 解析 `[CFG_INFO] role=... self_mac=... peer_mac=...|none ssid=...` 单行
///
/// 注意：ssid 字段当前 firmware 输出不允许包含空格（提取到下一空白即停），
/// 与 firmware 端 `wifi <SSID> <PASS>` 的 SSID 限制一致。
pub fn parse_cfg_info(line: &str) -> Option<CfgInfo> {
    let marker_idx = line.find("[CFG_INFO]")?;
    let after = &line[marker_idx + "[CFG_INFO]".len()..];

    let role = extract_kv(after, "role")?.to_string();
    if role != "master" && role != "slave" {
        return None;
    }
    let self_mac = extract_kv(after, "self_mac")?.to_string();
    let peer_raw = extract_kv(after, "peer_mac")?;
    let peer_mac = if peer_raw == "none" {
        None
    } else {
        Some(peer_raw.to_string())
    };
    let ssid = extract_kv(after, "ssid")?.to_string();

    // wifi_pwd 是 [CFG_INFO] v4 字段；老固件无此字段 = None，"none" 也视作 None
    let wifi_pwd = extract_kv(after, "wifi_pwd").and_then(|s| {
        if s == "none" || s.is_empty() {
            None
        } else {
            Some(s.to_string())
        }
    });

    // 以下三个字段是 [CFG_INFO] v2 新增；老固件无此字段时优雅降级为 false / None
    let wifi_connected = extract_kv(after, "wifi")
        .map(|s| s == "connected")
        .unwrap_or(false);
    let ip = extract_kv(after, "ip").and_then(|s| {
        if s == "none" || s.is_empty() {
            None
        } else {
            Some(s.to_string())
        }
    });
    let rssi = extract_kv(after, "rssi").and_then(|s| {
        if s == "none" {
            None
        } else {
            s.parse::<i32>().ok()
        }
    });
    // mode 是 [CFG_INFO] v3 字段；老固件无输出时为 None。
    // 仅接受白名单值，未知值（含未来新增模式名）也保留为字符串透传给前端。
    let mode = extract_kv(after, "mode").map(|s| s.to_string());

    Some(CfgInfo {
        role,
        self_mac,
        peer_mac,
        ssid,
        wifi_pwd,
        wifi_connected,
        ip,
        rssi,
        mode,
    })
}

// ---------------------- 单测 ----------------------

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parse_progress_overall_start() {
        let p = parse_cal_progress("[1234ms] [CAL] stage=overall phase=start").unwrap();
        assert_eq!(p.stage, CalStage::Overall);
        assert_eq!(p.phase, CalPhase::Start);
        assert!(p.remain.is_none() && p.ok.is_none() && p.reason.is_none() && p.flags.is_none());
    }

    #[test]
    fn parse_progress_countdown_remain() {
        for n in 1u8..=3u8 {
            let line = format!("[CAL] stage=imu phase=countdown remain={}", n);
            let p = parse_cal_progress(&line).unwrap();
            assert_eq!(p.stage, CalStage::Imu);
            assert_eq!(p.phase, CalPhase::Countdown);
            assert_eq!(p.remain, Some(n));
        }
    }

    #[test]
    fn parse_progress_sampling() {
        let p = parse_cal_progress("[CAL] stage=flex_min phase=sampling").unwrap();
        assert_eq!(p.stage, CalStage::FlexMin);
        assert_eq!(p.phase, CalPhase::Sampling);
        assert!(p.remain.is_none() && p.ok.is_none());
    }

    #[test]
    fn parse_progress_done_ok() {
        let p = parse_cal_progress("[CAL] stage=imu phase=done ok=1").unwrap();
        assert_eq!(p.stage, CalStage::Imu);
        assert_eq!(p.phase, CalPhase::Done);
        assert_eq!(p.ok, Some(true));
        assert!(p.reason.is_none());
    }

    #[test]
    fn parse_progress_done_fail_with_reason() {
        let p = parse_cal_progress(
            "[CAL] stage=flex_max phase=done ok=0 reason=range_too_small",
        )
        .unwrap();
        assert_eq!(p.stage, CalStage::FlexMax);
        assert_eq!(p.phase, CalPhase::Done);
        assert_eq!(p.ok, Some(false));
        assert_eq!(p.reason.as_deref(), Some("range_too_small"));
    }

    #[test]
    fn parse_progress_overall_done_with_flags() {
        let p =
            parse_cal_progress("[123ms] [CAL] stage=overall phase=done ok=1 flags=3").unwrap();
        assert_eq!(p.stage, CalStage::Overall);
        assert_eq!(p.phase, CalPhase::Done);
        assert_eq!(p.ok, Some(true));
        assert_eq!(p.flags, Some(3));
    }

    #[test]
    fn parse_progress_save_done() {
        let p = parse_cal_progress("[CAL] stage=save phase=done ok=1").unwrap();
        assert_eq!(p.stage, CalStage::Save);
        assert_eq!(p.phase, CalPhase::Done);
        assert_eq!(p.ok, Some(true));
    }

    #[test]
    fn parse_progress_unknown_stage_returns_none() {
        // 防御：未知 stage / phase 不应崩
        assert!(parse_cal_progress("[CAL] stage=foo phase=done ok=1").is_none());
        assert!(parse_cal_progress("[CAL] stage=imu phase=bar").is_none());
    }

    #[test]
    fn parse_progress_does_not_match_cal_info() {
        // 避免 `[CAL_INFO]` 行被进度解析器误吃（因为 `[CAL]` 是 `[CAL_INFO]` 的真子串）
        assert!(parse_cal_progress(
            "[CAL_INFO] flags=3 ax=0.01 ay=0.0 az=0.0 gx=0.0 gy=0.0 gz=0.0 \
             fmin=1100,1090,1110,1080,1095 fmax=2780,2810,2790,2820,2800"
        )
        .is_none());
    }

    #[test]
    fn parse_progress_garbage_lines() {
        assert!(parse_cal_progress("").is_none());
        assert!(parse_cal_progress("12345,1.0,2.0,3.0,4.0").is_none());
        assert!(parse_cal_progress("[校准] 步骤 1/3: IMU 零偏").is_none());
        assert!(parse_cal_progress("[配置] NVS 角色: MASTER").is_none());
    }

    #[test]
    fn parse_info_full() {
        let line = "[567ms] [CAL_INFO] flags=3 ax=0.0123 ay=-0.0050 az=0.0110 \
                    gx=0.2000 gy=-0.1000 gz=0.0500 \
                    fmin=1100,1090,1110,1080,1095 fmax=2780,2810,2790,2820,2800";
        let info = parse_cal_info(line).unwrap();
        assert_eq!(info.flags, 3);
        assert!((info.accel_bias[0] - 0.0123).abs() < 1e-5);
        assert!((info.accel_bias[1] - (-0.005)).abs() < 1e-5);
        assert!((info.gyro_bias[2] - 0.05).abs() < 1e-5);
        assert_eq!(info.flex_min, [1100, 1090, 1110, 1080, 1095]);
        assert_eq!(info.flex_max, [2780, 2810, 2790, 2820, 2800]);
    }

    #[test]
    fn parse_info_uncalibrated() {
        // flags=0：所有字段都是 0 但仍能解析
        let line = "[CAL_INFO] flags=0 ax=0.0000 ay=0.0000 az=0.0000 \
                    gx=0.0000 gy=0.0000 gz=0.0000 \
                    fmin=0,0,0,0,0 fmax=0,0,0,0,0";
        let info = parse_cal_info(line).unwrap();
        assert_eq!(info.flags, 0);
        assert_eq!(info.accel_bias, [0.0, 0.0, 0.0]);
        assert_eq!(info.flex_min, [0; 5]);
        assert_eq!(info.flex_max, [0; 5]);
    }

    #[test]
    fn parse_info_imu_only_flags1() {
        // 仅 IMU 校准：flex_min/max 仍打印为 0（端侧约定字段总是齐全）
        let line = "[CAL_INFO] flags=1 ax=0.012 ay=0.0 az=0.011 gx=0.2 gy=0.0 gz=0.05 \
                    fmin=0,0,0,0,0 fmax=0,0,0,0,0";
        let info = parse_cal_info(line).unwrap();
        assert_eq!(info.flags, 1);
        assert!((info.accel_bias[0] - 0.012).abs() < 1e-5);
        assert_eq!(info.flex_min, [0; 5]);
    }

    #[test]
    fn parse_info_without_flex_section_disabled_firmware() {
        // ENABLE_FLEX_SENSORS=0 时 firmware 不输出 fmin/fmax；解析器降级为 0
        let line = "[CAL_INFO] flags=1 ax=0.01 ay=0.0 az=0.0 gx=0.0 gy=0.0 gz=0.0";
        let info = parse_cal_info(line).unwrap();
        assert_eq!(info.flags, 1);
        assert_eq!(info.flex_min, [0; 5]);
        assert_eq!(info.flex_max, [0; 5]);
    }

    #[test]
    fn parse_info_garbage() {
        assert!(parse_cal_info("").is_none());
        // 缺关键字段
        assert!(parse_cal_info("[CAL_INFO] flags=3").is_none());
        // 非数值
        assert!(parse_cal_info(
            "[CAL_INFO] flags=abc ax=0.0 ay=0.0 az=0.0 gx=0.0 gy=0.0 gz=0.0"
        )
        .is_none());
    }

    #[test]
    fn extract_kv_does_not_confuse_substring_keys() {
        // "ax=" 不应被 "fmax=" 误命中
        let line = "[CAL_INFO] flags=3 ax=0.01 ay=0.02 az=0.03 \
                    gx=0.0 gy=0.0 gz=0.0 fmin=1,2,3,4,5 fmax=6,7,8,9,10";
        let info = parse_cal_info(line).unwrap();
        assert!((info.accel_bias[0] - 0.01).abs() < 1e-5);
        assert_eq!(info.flex_max, [6, 7, 8, 9, 10]);
    }

    // ---------------- CFG_INFO ----------------

    #[test]
    fn parse_cfg_info_master_with_peer() {
        let line = "[1234ms] [CFG_INFO] role=master self_mac=AA:BB:CC:DD:EE:FF \
                    peer_mac=11:22:33:44:55:66 ssid=lingxi_test";
        let cfg = parse_cfg_info(line).unwrap();
        assert_eq!(cfg.role, "master");
        assert_eq!(cfg.self_mac, "AA:BB:CC:DD:EE:FF");
        assert_eq!(cfg.peer_mac.as_deref(), Some("11:22:33:44:55:66"));
        assert_eq!(cfg.ssid, "lingxi_test");
    }

    #[test]
    fn parse_cfg_info_slave_no_peer() {
        let line = "[CFG_INFO] role=slave self_mac=00:11:22:33:44:55 \
                    peer_mac=none ssid=open_wifi";
        let cfg = parse_cfg_info(line).unwrap();
        assert_eq!(cfg.role, "slave");
        assert!(cfg.peer_mac.is_none());
        assert_eq!(cfg.ssid, "open_wifi");
    }

    #[test]
    fn parse_cfg_info_invalid_role() {
        let line = "[CFG_INFO] role=unknown self_mac=AA:.. peer_mac=none ssid=x";
        assert!(parse_cfg_info(line).is_none());
    }

    #[test]
    fn parse_cfg_info_missing_field() {
        // 缺 ssid
        let line = "[CFG_INFO] role=master self_mac=AA:BB:CC:DD:EE:FF peer_mac=none";
        assert!(parse_cfg_info(line).is_none());
        // 完全空行
        assert!(parse_cfg_info("").is_none());
        // 不带 marker
        assert!(parse_cfg_info("role=master").is_none());
    }

    #[test]
    fn parse_cfg_info_does_not_match_cal_info() {
        // [CAL_INFO] 不应被 cfg 解析器误吃
        let line = "[CAL_INFO] flags=3 ax=0.01 ay=0.0 az=0.0 \
                    gx=0.0 gy=0.0 gz=0.0 fmin=0,0,0,0,0 fmax=0,0,0,0,0";
        assert!(parse_cfg_info(line).is_none());
    }

    #[test]
    fn parse_cfg_info_v2_with_wifi_ip_rssi() {
        let line = "[CFG_INFO] role=master self_mac=AA:BB:CC:DD:EE:FF \
                    peer_mac=11:22:33:44:55:66 ssid=lingxi_test \
                    wifi=connected ip=192.168.1.105 rssi=-58";
        let cfg = parse_cfg_info(line).unwrap();
        assert!(cfg.wifi_connected);
        assert_eq!(cfg.ip.as_deref(), Some("192.168.1.105"));
        assert_eq!(cfg.rssi, Some(-58));
    }

    #[test]
    fn parse_cfg_info_v2_wifi_disconnected() {
        let line = "[CFG_INFO] role=slave self_mac=00:11:22:33:44:55 \
                    peer_mac=none ssid=any wifi=disconnected ip=none rssi=none";
        let cfg = parse_cfg_info(line).unwrap();
        assert!(!cfg.wifi_connected);
        assert!(cfg.ip.is_none());
        assert!(cfg.rssi.is_none());
    }

    #[test]
    fn parse_cfg_info_v1_legacy_falls_back() {
        // 老固件不输出 wifi/ip/rssi/mode，回退为 false / None
        let line = "[CFG_INFO] role=master self_mac=AA:BB:CC:DD:EE:FF \
                    peer_mac=none ssid=foo";
        let cfg = parse_cfg_info(line).unwrap();
        assert!(!cfg.wifi_connected);
        assert!(cfg.ip.is_none());
        assert!(cfg.rssi.is_none());
        assert!(cfg.mode.is_none());
    }

    #[test]
    fn parse_cfg_info_v3_mode_recognize() {
        let line = "[CFG_INFO] role=master self_mac=AA:BB:CC:DD:EE:FF \
                    peer_mac=11:22:33:44:55:66 ssid=lingxi_test \
                    wifi=connected ip=192.168.1.105 rssi=-58 mode=recognize";
        let cfg = parse_cfg_info(line).unwrap();
        assert_eq!(cfg.mode.as_deref(), Some("recognize"));
    }

    #[test]
    fn parse_cfg_info_v3_mode_capture() {
        let line = "[CFG_INFO] role=slave self_mac=00:11:22:33:44:55 \
                    peer_mac=none ssid=any wifi=disconnected ip=none rssi=none \
                    mode=capture";
        let cfg = parse_cfg_info(line).unwrap();
        assert_eq!(cfg.mode.as_deref(), Some("capture"));
    }

    #[test]
    fn parse_cfg_info_v3_mode_finger_spelling() {
        let line = "[CFG_INFO] role=master self_mac=AA:BB:CC:DD:EE:FF \
                    peer_mac=none ssid=x wifi=disconnected ip=none rssi=none \
                    mode=finger_spelling";
        let cfg = parse_cfg_info(line).unwrap();
        assert_eq!(cfg.mode.as_deref(), Some("finger_spelling"));
    }
}
