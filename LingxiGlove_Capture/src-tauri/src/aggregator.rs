//! Frame 汇聚器
//!
//! **职责**：从多个 SerialTask 的 mpsc::Receiver 收 Frame → 汇聚 →
//! 1. 注入当前 label（来自 LabelState）
//! 2. 录制中写入 SessionState 对应 alias 的 raw.csv
//! 3. 累积帧率统计
//! 4. emit("frame") 推前端
//!
//! **故障隔离**：单个写盘错误只 log，不退出 aggregator；任何一个 SerialTask
//! 关闭只让对应 Sender drop，不影响其他设备。

use crate::label::SharedLabelState;
use crate::session::SessionState;
use crate::types::Frame;
use std::collections::HashMap;
use std::sync::{Arc, Mutex};
use std::time::Instant;
use tauri::{AppHandle, Emitter};
use tokio::sync::mpsc;

/// 帧率统计窗口（最近 1 秒）
#[derive(Default, Debug, Clone)]
pub struct FpsCounter {
    pub frame_count: u64,
    pub fps: f32,
    last_window_start: Option<Instant>,
    last_window_count: u64,
}

impl FpsCounter {
    pub fn tick(&mut self) {
        self.frame_count += 1;
        let now = Instant::now();
        match self.last_window_start {
            None => {
                self.last_window_start = Some(now);
                self.last_window_count = self.frame_count;
            }
            Some(start) => {
                let elapsed = now.duration_since(start).as_secs_f32();
                if elapsed >= 1.0 {
                    let delta = self.frame_count - self.last_window_count;
                    self.fps = delta as f32 / elapsed;
                    self.last_window_start = Some(now);
                    self.last_window_count = self.frame_count;
                }
            }
        }
    }
}

/// per-device 帧率统计
pub type FpsMap = Arc<Mutex<HashMap<String, FpsCounter>>>;

/// 录制会话（None 表示当前未录制）
pub type SharedSession = Arc<Mutex<Option<SessionState>>>;

/// 启动 aggregator 后台 task。
pub fn spawn_aggregator(
    mut rx: mpsc::Receiver<Frame>,
    app: AppHandle,
    fps_map: FpsMap,
    label_state: SharedLabelState,
    session: SharedSession,
) {
    tauri::async_runtime::spawn(async move {
        log::info!("aggregator started");
        while let Some(mut frame) = rx.recv().await {
            // 1. 注入当前 label（PC 端权威）
            frame.label = label_state.get();

            // 2. 帧率统计
            {
                let mut map = fps_map.lock().unwrap();
                map.entry(frame.dev_alias.clone())
                    .or_default()
                    .tick();
            }

            // 3. 录制中：写盘（错误只 log，不中断流）
            //    锁尽量短：先锁 session 写完一帧立即释放
            //    路由由 SessionState::on_frame_obj 内部处理：
            //      - bimanual_raw=Some  → bimanual writer
            //      - 普通单手帧         → alias writer
            //      - bimanual 拆出的右帧 → 静默忽略
            {
                let mut sess_guard = session.lock().unwrap();
                if let Some(sess) = sess_guard.as_mut() {
                    if let Err(e) = sess.on_frame_obj(&frame) {
                        log::error!(
                            "[{}] session write error: {}",
                            frame.dev_alias,
                            e
                        );
                    }
                }
            }

            // 4. 推给前端
            //    Day 2 单帧推送够用（20fps × 2 设备 = 40Hz emit）；Day 3 若卡顿改批量。
            if let Err(e) = app.emit("frame", &frame) {
                log::warn!("emit frame failed: {}", e);
            }
        }
        log::info!("aggregator exited (all serial tasks dropped)");
    });
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn fps_counter_basic() {
        let mut c = FpsCounter::default();
        c.tick();
        c.tick();
        assert_eq!(c.frame_count, 2);
        // fps 在不到 1 秒内不更新，保持 0
        assert_eq!(c.fps, 0.0);
    }
}
