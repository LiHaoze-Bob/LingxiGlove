//! Day 3 数据流水线模块
//!
//! 职责：
//! - 列举 out_root 下的 session_*/raw.csv（前端 SessionPanel 用）
//! - 调用 `python tools/build_dataset.py` 切窗（异步 spawn + emit 进度）
//! - 把 `dataset/ei_csv/{train,test}/*.csv` 逐个 POST 到 Edge Impulse Ingestion API
//! - 一键流水线：build_dataset → upload，统一 emit 同一个事件名 `pipeline-progress`
//!
//! 事件模型：
//!   `pipeline-progress` payload = { stage, message, current, total, ok }
//!   stage ∈ "build_dataset_stdout" / "build_dataset_done" /
//!           "upload_start" / "upload_progress" / "upload_done"
//!
//! 错误处理：每个 command 返回 `Result<_, String>`，前端 toast。

use crate::secrets;
use serde::{Deserialize, Serialize};
use std::path::{Path, PathBuf};
use std::process::Stdio;
use tauri::{AppHandle, Emitter};
use tokio::io::{AsyncBufReadExt, BufReader};
use tokio::process::Command as TokioCommand;

// ---------- 类型 ----------

/// 单条会话条目（list_sessions 返回）
#[derive(Debug, Serialize)]
pub struct SessionEntry {
    pub session_id: String,
    /// session 目录绝对路径
    pub path: String,
    /// raw.csv 绝对路径
    pub raw_csv: String,
    /// 数据行数（不含 header；读取失败时为 0）
    pub rows: u64,
    /// 已打标行数（label != -1）
    pub labeled_rows: u64,
    /// 各 label 出现次数（按 label 升序，-1 在最前）
    /// 序列化为 `[[-1,19],[0,100],[1,80]]`，前端按 LABEL_NAMES 翻译展示
    pub label_counts: Vec<(i8, u64)>,
}

/// build_dataset 调用参数
#[derive(Debug, Deserialize)]
pub struct BuildDatasetArgs {
    /// 输入根（含 session_*/raw.csv），通常 = AppState.out_root
    pub in_root: String,
    /// 输出根（含 ei_csv/）
    pub out_root: String,
    /// build_dataset.py 绝对路径（前端从设置中传入）
    pub script_path: String,
    /// python 解释器路径（默认 "python3"）
    pub python: Option<String>,
    /// 透传给 script 的额外参数（例如 ["--window","20","--stride","10"]）
    #[serde(default)]
    pub extra_args: Vec<String>,
    /// 可选：仅处理指定的 session_id 列表。
    /// 不传或为空则处理 in_root 下全部 session（保持原有行为）。
    #[serde(default)]
    pub session_ids: Option<Vec<String>>,
}

/// 子进程退出统一摘要（前端 toast 用）
#[derive(Debug, Serialize)]
pub struct ProcessOutcome {
    pub ok: bool,
    pub exit_code: i32,
    pub stdout_lines: usize,
    pub stderr_lines: usize,
}

/// EI 上传单文件结果（仅汇总在 emit 中，命令本身只返回总览）
#[derive(Debug, Serialize)]
pub struct UploadOutcome {
    pub total: usize,
    pub uploaded: usize,
    pub failed: usize,
}

// ---------- list_sessions ----------

#[tauri::command]
pub fn list_sessions(out_root: String) -> Result<Vec<SessionEntry>, String> {
    let root = PathBuf::from(out_root);
    if !root.exists() {
        return Ok(Vec::new());
    }
    let mut out = Vec::new();
    let dir = std::fs::read_dir(&root).map_err(|e| format!("read_dir {}: {}", root.display(), e))?;
    for ent in dir.flatten() {
        let p = ent.path();
        if !p.is_dir() {
            continue;
        }
        let name = match p.file_name().and_then(|s| s.to_str()) {
            Some(s) if s.starts_with("session_") => s.to_string(),
            _ => continue,
        };
        let raw = p.join("raw.csv");
        if !raw.exists() {
            continue;
        }
        let (rows, labeled, label_counts) = count_rows(&raw);
        out.push(SessionEntry {
            session_id: name,
            path: p.to_string_lossy().to_string(),
            raw_csv: raw.to_string_lossy().to_string(),
            rows,
            labeled_rows: labeled,
            label_counts,
        });
    }
    // 按 session_id 倒序（最近的在前）
    out.sort_by(|a, b| b.session_id.cmp(&a.session_id));
    Ok(out)
}

/// 读 raw.csv 统计 (行数, 已打标行数, 各 label 计数)；失败返回 (0,0,vec![])
///
/// label_counts 用 BTreeMap 累加再转 Vec，输出按 label 升序（-1 在最前）。
/// 非法 / 解析失败的 label 行计入总行数但不计入 label_counts。
fn count_rows(path: &Path) -> (u64, u64, Vec<(i8, u64)>) {
    use std::collections::BTreeMap;
    use std::io::BufRead;
    let f = match std::fs::File::open(path) {
        Ok(f) => f,
        Err(_) => return (0, 0, Vec::new()),
    };
    let reader = std::io::BufReader::new(f);
    let mut total: u64 = 0;
    let mut labeled: u64 = 0;
    let mut counts: BTreeMap<i8, u64> = BTreeMap::new();
    for (i, line) in reader.lines().enumerate() {
        let line = match line {
            Ok(l) => l,
            Err(_) => break,
        };
        if i == 0 {
            continue; // header
        }
        if line.is_empty() {
            continue;
        }
        total += 1;
        // 最后一列是 label
        if let Some(last) = line.rsplit(',').next() {
            let s = last.trim();
            if s != "-1" {
                labeled += 1;
            }
            if let Ok(v) = s.parse::<i8>() {
                *counts.entry(v).or_insert(0) += 1;
            }
        }
    }
    (total, labeled, counts.into_iter().collect())
}

// ---------- delete_session ----------

/// 删除一个会话目录（session_*）。
///
/// 严格校验：
/// 1. session_id 必须以 "session_" 开头
/// 2. session_id 不能含路径分隔符（'/' 或 '\\'）或 ".."
/// 3. 路径规范后必须仍在 out_root 下
/// 4. 路径实际必须存在且是目录
#[tauri::command]
pub fn delete_session(out_root: String, session_id: String) -> Result<(), String> {
    if !session_id.starts_with("session_") {
        return Err(format!("非法 session_id（需以 session_ 开头）: {}", session_id));
    }
    if session_id.contains('/') || session_id.contains('\\') || session_id.contains("..") {
        return Err(format!("非法 session_id（含非法字符）: {}", session_id));
    }

    let root = PathBuf::from(&out_root);
    let target = root.join(&session_id);

    // 防范路径逐层向上逸出：要求 target 规范后仍 starts_with(out_root 规范后)
    let canon_root = root
        .canonicalize()
        .map_err(|e| format!("out_root 不存在或不可访问 {}: {}", root.display(), e))?;
    let canon_target = target
        .canonicalize()
        .map_err(|e| format!("session 不存在或不可访问 {}: {}", target.display(), e))?;
    if !canon_target.starts_with(&canon_root) {
        return Err(format!(
            "拒绝删除：session 路径不在 out_root 下（{} not in {}）",
            canon_target.display(),
            canon_root.display()
        ));
    }
    if !canon_target.is_dir() {
        return Err(format!("路径不是目录: {}", canon_target.display()));
    }

    std::fs::remove_dir_all(&canon_target)
        .map_err(|e| format!("删除 {} 失败: {}", canon_target.display(), e))?;
    log::info!("deleted session: {}", canon_target.display());
    Ok(())
}

// ---------- run_build_dataset ----------

/// 运行 `python build_dataset.py --in <in> --out <out> ...`
/// 把 stdout/stderr 逐行 emit 到前端事件 `pipeline-progress`
#[tauri::command]
pub async fn run_build_dataset(
    app: AppHandle,
    args: BuildDatasetArgs,
) -> Result<ProcessOutcome, String> {
    let python = args.python.unwrap_or_else(|| "python3".to_string());
    let script = PathBuf::from(&args.script_path);
    if !script.exists() {
        return Err(format!("找不到 build_dataset.py: {}", script.display()));
    }

    let mut cmd = TokioCommand::new(&python);
    cmd.arg(&script)
        .arg("--in")
        .arg(&args.in_root)
        .arg("--out")
        .arg(&args.out_root)
        .args(&args.extra_args);

    // 会话过滤：仅在前端传入非空列表时追加 --sessions
    let session_ids = args.session_ids.clone().unwrap_or_default();
    if !session_ids.is_empty() {
        cmd.arg("--sessions");
        for sid in &session_ids {
            cmd.arg(sid);
        }
    }

    cmd.stdout(Stdio::piped()).stderr(Stdio::piped());

    log::info!(
        "spawn: {} {} --in {} --out {} sessions={:?} extras={:?}",
        python,
        script.display(),
        args.in_root,
        args.out_root,
        session_ids,
        args.extra_args
    );

    let mut child = cmd
        .spawn()
        .map_err(|e| format!("spawn {} 失败：{}", python, e))?;
    let stdout = child.stdout.take().ok_or("无法获取 stdout")?;
    let stderr = child.stderr.take().ok_or("无法获取 stderr")?;

    let app_for_stdout = app.clone();
    let stdout_handle = tokio::spawn(async move {
        let mut count = 0usize;
        let mut r = BufReader::new(stdout).lines();
        while let Ok(Some(line)) = r.next_line().await {
            count += 1;
            let _ = app_for_stdout.emit(
                "pipeline-progress",
                ProgressEvent::log("build_dataset_stdout", &line),
            );
        }
        count
    });

    let app_for_stderr = app.clone();
    let stderr_handle = tokio::spawn(async move {
        let mut count = 0usize;
        let mut r = BufReader::new(stderr).lines();
        while let Ok(Some(line)) = r.next_line().await {
            count += 1;
            let _ = app_for_stderr.emit(
                "pipeline-progress",
                ProgressEvent::log("build_dataset_stderr", &line),
            );
        }
        count
    });

    let status = child
        .wait()
        .await
        .map_err(|e| format!("等待子进程失败：{}", e))?;
    let stdout_lines = stdout_handle.await.unwrap_or(0);
    let stderr_lines = stderr_handle.await.unwrap_or(0);

    let exit_code = status.code().unwrap_or(-1);
    let ok = status.success();
    let _ = app.emit(
        "pipeline-progress",
        ProgressEvent::done(
            "build_dataset_done",
            ok,
            &format!("exit={} stdout={} stderr={}", exit_code, stdout_lines, stderr_lines),
        ),
    );
    Ok(ProcessOutcome {
        ok,
        exit_code,
        stdout_lines,
        stderr_lines,
    })
}

// ---------- upload_to_ei ----------

/// 上传 dataset 根下 `ei_csv/train/*.csv` + `ei_csv/test/*.csv` 到 EI
///
/// 文件名约定（来自 build_dataset.py）：`<label_name>.<seq>.csv`
/// → 用 `.` 之前的 segment 作为 EI 的 `x-label`
#[tauri::command]
pub async fn upload_to_ei(
    app: AppHandle,
    dataset_root: String,
) -> Result<UploadOutcome, String> {
    let key = secrets::get_ei_key()?
        .ok_or_else(|| "未配置 Edge Impulse API Key（请在设置中填写）".to_string())?;
    let root = PathBuf::from(&dataset_root);
    let train_dir = root.join("ei_csv").join("train");
    let test_dir = root.join("ei_csv").join("test");

    let train_files = list_csv(&train_dir);
    let test_files = list_csv(&test_dir);
    let total = train_files.len() + test_files.len();
    if total == 0 {
        return Err(format!(
            "{}/ei_csv/{{train,test}} 下未发现 CSV，请先跑 build_dataset",
            root.display()
        ));
    }

    let _ = app.emit(
        "pipeline-progress",
        ProgressEvent::progress(
            "upload_start",
            &format!("准备上传 {} 个文件 (train={}, test={})", total, train_files.len(), test_files.len()),
            0,
            total,
        ),
    );

    let client = reqwest::Client::builder()
        .build()
        .map_err(|e| format!("init http client: {}", e))?;
    let mut uploaded = 0usize;
    let mut failed = 0usize;
    let mut idx = 0usize;

    for (cat, files) in [("training", train_files), ("testing", test_files)] {
        for path in files {
            idx += 1;
            let label = parse_label_from_filename(&path);
            let outcome = upload_one(&client, &key, cat, &path, &label).await;
            let msg = match &outcome {
                Ok(_) => {
                    uploaded += 1;
                    format!("[{}/{}] ✓ {} ({})", idx, total, file_short(&path), cat)
                }
                Err(e) => {
                    failed += 1;
                    format!("[{}/{}] ✗ {}: {}", idx, total, file_short(&path), e)
                }
            };
            let _ = app.emit(
                "pipeline-progress",
                ProgressEvent::progress("upload_progress", &msg, idx, total),
            );
        }
    }

    let _ = app.emit(
        "pipeline-progress",
        ProgressEvent::done(
            "upload_done",
            failed == 0,
            &format!("done: {}/{} uploaded, {} failed", uploaded, total, failed),
        ),
    );
    Ok(UploadOutcome {
        total,
        uploaded,
        failed,
    })
}

fn list_csv(dir: &Path) -> Vec<PathBuf> {
    let mut out = Vec::new();
    if let Ok(rd) = std::fs::read_dir(dir) {
        for ent in rd.flatten() {
            let p = ent.path();
            if p.is_file() && p.extension().map(|e| e == "csv").unwrap_or(false) {
                out.push(p);
            }
        }
    }
    out.sort();
    out
}

fn file_short(p: &Path) -> String {
    p.file_name()
        .map(|s| s.to_string_lossy().to_string())
        .unwrap_or_else(|| p.display().to_string())
}

/// 文件名格式 `<label>.<seq>.csv` → 取第一个 `.` 前作为 label
fn parse_label_from_filename(path: &Path) -> String {
    let name = path
        .file_stem()
        .and_then(|s| s.to_str())
        .unwrap_or("unlabeled");
    match name.split_once('.') {
        Some((label, _)) => label.to_string(),
        None => name.to_string(),
    }
}

async fn upload_one(
    client: &reqwest::Client,
    api_key: &str,
    category: &str, // "training" or "testing"
    path: &Path,
    label: &str,
) -> Result<(), String> {
    let url = format!("https://ingestion.edgeimpulse.com/api/{}/files", category);
    let bytes = tokio::fs::read(path)
        .await
        .map_err(|e| format!("read {}: {}", path.display(), e))?;
    let part = reqwest::multipart::Part::bytes(bytes)
        .file_name(file_short(path))
        .mime_str("text/csv")
        .map_err(|e| format!("mime: {}", e))?;
    let form = reqwest::multipart::Form::new().part("data", part);

    let resp = client
        .post(&url)
        .header("x-api-key", api_key)
        .header("x-label", label)
        .header("x-disallow-duplicates", "1")
        .multipart(form)
        .send()
        .await
        .map_err(|e| format!("POST {}: {}", url, e))?;

    let status = resp.status();
    if status.is_success() {
        Ok(())
    } else {
        let body = resp.text().await.unwrap_or_default();
        let snippet: String = body.chars().take(200).collect();
        Err(format!("HTTP {}: {}", status.as_u16(), snippet))
    }
}

// ---------- run_pipeline (build + upload) ----------

#[tauri::command]
pub async fn run_pipeline(
    app: AppHandle,
    build_args: BuildDatasetArgs,
    dataset_root: String,
) -> Result<PipelineOutcome, String> {
    // 1) build_dataset
    let build = run_build_dataset(app.clone(), build_args).await?;
    if !build.ok {
        return Ok(PipelineOutcome {
            build,
            upload: None,
        });
    }
    // 2) upload
    let upload = upload_to_ei(app.clone(), dataset_root).await?;
    Ok(PipelineOutcome {
        build,
        upload: Some(upload),
    })
}

#[derive(Debug, Serialize)]
pub struct PipelineOutcome {
    pub build: ProcessOutcome,
    pub upload: Option<UploadOutcome>,
}

// ---------- Secrets command 入口 ----------

#[tauri::command]
pub fn set_ei_key(key: String) -> Result<(), String> {
    secrets::set_ei_key(&key)
}

#[tauri::command]
pub fn has_ei_key() -> bool {
    secrets::has_ei_key()
}

#[tauri::command]
pub fn delete_ei_key() -> Result<(), String> {
    secrets::delete_ei_key()
}

// ---------- 进度事件 ----------

#[derive(Debug, Serialize, Clone)]
struct ProgressEvent {
    stage: String,
    message: String,
    /// 0..=total，仅 progress 阶段有意义
    current: usize,
    total: usize,
    /// 仅 *_done 阶段有意义
    ok: Option<bool>,
}

impl ProgressEvent {
    fn log(stage: &str, msg: &str) -> Self {
        Self {
            stage: stage.into(),
            message: msg.into(),
            current: 0,
            total: 0,
            ok: None,
        }
    }
    fn progress(stage: &str, msg: &str, current: usize, total: usize) -> Self {
        Self {
            stage: stage.into(),
            message: msg.into(),
            current,
            total,
            ok: None,
        }
    }
    fn done(stage: &str, ok: bool, msg: &str) -> Self {
        Self {
            stage: stage.into(),
            message: msg.into(),
            current: 0,
            total: 0,
            ok: Some(ok),
        }
    }
}

// ---------- 单测 ----------

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parse_label_from_simple() {
        let p = PathBuf::from("/x/y/half.0001.csv");
        assert_eq!(parse_label_from_filename(&p), "half");
    }

    #[test]
    fn parse_label_no_dot_seq() {
        let p = PathBuf::from("/x/y/full.csv");
        assert_eq!(parse_label_from_filename(&p), "full");
    }

    #[test]
    fn list_sessions_filters_non_session_dirs() {
        let tmp = tempfile::tempdir().unwrap();
        let root = tmp.path();
        // 三个目录：session_a 含 raw.csv / session_b 不含 / random_dir
        let a = root.join("session_20260101_120000_left");
        std::fs::create_dir_all(&a).unwrap();
        std::fs::write(
            a.join("raw.csv"),
            "timestamp_ms,ax,ay,az,gx,gy,gz,pitch,roll,flex0,flex1,flex2,flex3,flex4,label\n\
             0,0,0,0,0,0,0,0,0,0,0,0,0,0,0\n\
             50,0,0,0,0,0,0,0,0,0,0,0,0,0,-1\n",
        )
        .unwrap();
        let b = root.join("session_20260101_120100_right");
        std::fs::create_dir_all(&b).unwrap();
        let c = root.join("not_session");
        std::fs::create_dir_all(&c).unwrap();

        let mut entries = list_sessions(root.to_string_lossy().to_string()).unwrap();
        entries.sort_by(|a, b| a.session_id.cmp(&b.session_id));
        assert_eq!(entries.len(), 1);
        assert_eq!(entries[0].rows, 2);
        assert_eq!(entries[0].labeled_rows, 1);
    }
}
