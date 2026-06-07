//! 全局 label 广播
//!
//! Day 2 PC 端打标核心：单一全局 i8，由前端 keydown 通过 `set_label` command
//! 实时切换；Aggregator 在每帧上注入 `frame.label = current()`。
//!
//! - 默认 -1（unlabeled）
//! - 0..=9：与端侧 `CAPTURE_LABEL_NAMES[]` 对齐，写盘时即覆盖端侧 label 列
//! - 由于操作粒度极小且无需阻塞读串口，直接 `Mutex<i8>`，不引入更复杂的同步原语

use std::sync::{Arc, Mutex};

#[derive(Debug, Default)]
pub struct LabelState {
    label: Mutex<i8>,
}

impl LabelState {
    pub fn new() -> Self {
        Self {
            label: Mutex::new(-1),
        }
    }

    pub fn set(&self, label: i8) {
        *self.label.lock().unwrap() = label;
    }

    pub fn get(&self) -> i8 {
        *self.label.lock().unwrap()
    }
}

pub type SharedLabelState = Arc<LabelState>;

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn default_is_unlabeled() {
        let s = LabelState::new();
        assert_eq!(s.get(), -1);
    }

    #[test]
    fn set_and_get() {
        let s = LabelState::new();
        s.set(3);
        assert_eq!(s.get(), 3);
        s.set(-1);
        assert_eq!(s.get(), -1);
    }
}
