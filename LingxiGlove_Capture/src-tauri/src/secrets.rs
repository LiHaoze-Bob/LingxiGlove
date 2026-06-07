//! macOS Keychain 持久化 EI API Key
//!
//! 使用 `keyring` crate 的 apple-native backend，存到登录钥匙串：
//!   service: "lingxi-capture"
//!   user:    "edge-impulse-api-key"
//!
//! 故障策略：keyring 任何错误都返回 String（前端 toast 展示），不 panic。

const SERVICE: &str = "lingxi-capture";
const ACCOUNT_EI: &str = "edge-impulse-api-key";

fn entry_ei() -> Result<keyring::Entry, String> {
    keyring::Entry::new(SERVICE, ACCOUNT_EI).map_err(|e| format!("keyring entry: {}", e))
}

/// 写入 EI api key（覆盖旧值）
pub fn set_ei_key(key: &str) -> Result<(), String> {
    if key.trim().is_empty() {
        return Err("api key 不能为空".to_string());
    }
    let e = entry_ei()?;
    e.set_password(key).map_err(|e| format!("keyring set: {}", e))?;
    Ok(())
}

/// 读取 EI api key（不存在返回 None）
pub fn get_ei_key() -> Result<Option<String>, String> {
    let e = entry_ei()?;
    match e.get_password() {
        Ok(s) => Ok(Some(s)),
        Err(keyring::Error::NoEntry) => Ok(None),
        Err(err) => Err(format!("keyring get: {}", err)),
    }
}

/// 删除 EI api key
pub fn delete_ei_key() -> Result<(), String> {
    let e = entry_ei()?;
    match e.delete_credential() {
        Ok(()) => Ok(()),
        Err(keyring::Error::NoEntry) => Ok(()),
        Err(err) => Err(format!("keyring delete: {}", err)),
    }
}

/// 仅判断是否已设置（前端"是否已配置"提示）
pub fn has_ei_key() -> bool {
    matches!(get_ei_key(), Ok(Some(_)))
}
