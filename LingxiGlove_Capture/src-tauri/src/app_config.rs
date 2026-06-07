//! 应用级用户偏好持久化
//!
//! 存放路径：`<app_data>/app_config.json`
//!
//! 当前仅持久化 `out_root`（用户在前端 dialog 选择的会话输出目录）；
//! 后续若有更多偏好（默认 EI 项目 ID、默认 Python 路径等）可按需扩展。

use serde::{Deserialize, Serialize};
use std::path::{Path, PathBuf};

const CONFIG_FILE: &str = "app_config.json";

#[derive(Debug, Default, Clone, Serialize, Deserialize)]
pub struct AppConfig {
    /// 用户自定义的会话输出根目录；None = 使用默认（<app_data>/output/capture）
    #[serde(default)]
    pub out_root: Option<PathBuf>,
}

impl AppConfig {
    /// 配置文件绝对路径
    pub fn path(app_data_dir: &Path) -> PathBuf {
        app_data_dir.join(CONFIG_FILE)
    }

    /// 从 `<app_data>/app_config.json` 读取，文件不存在或解析失败时返回默认值
    pub fn load(app_data_dir: &Path) -> Self {
        let p = Self::path(app_data_dir);
        match std::fs::read_to_string(&p) {
            Ok(content) => match serde_json::from_str::<AppConfig>(&content) {
                Ok(cfg) => {
                    log::info!("loaded app_config from {}", p.display());
                    cfg
                }
                Err(e) => {
                    log::warn!("parse app_config failed ({}), use default", e);
                    Self::default()
                }
            },
            Err(_) => Self::default(),
        }
    }

    /// 写回 `<app_data>/app_config.json`
    pub fn save(&self, app_data_dir: &Path) -> std::io::Result<()> {
        std::fs::create_dir_all(app_data_dir)?;
        let p = Self::path(app_data_dir);
        let content = serde_json::to_string_pretty(self).map_err(|e| {
            std::io::Error::new(std::io::ErrorKind::InvalidData, e.to_string())
        })?;
        std::fs::write(&p, content)?;
        log::info!("saved app_config to {}", p.display());
        Ok(())
    }
}
