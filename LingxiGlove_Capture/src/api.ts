/**
 * Tauri command 调用封装
 * 集中所有 invoke 调用，便于将来加 mock / error mapping。
 */
import { invoke } from "@tauri-apps/api/core";
import type {
  DeviceMeta,
  FpsSnapshot,
  SerialPortInfo,
  SessionInfo,
  SessionSummary,
  SessionEntry,
  BuildDatasetArgs,
  ProcessOutcome,
  UploadOutcome,
  PipelineOutcome,
} from "./types";

export async function listPorts(): Promise<SerialPortInfo[]> {
  return invoke<SerialPortInfo[]>("list_ports");
}

export async function connectDevice(
  alias: string,
  port: string,
  baud: number
): Promise<void> {
  await invoke("connect_device", { alias, port, baud });
}

export async function disconnectDevice(alias: string): Promise<void> {
  await invoke("disconnect_device", { alias });
}

/**
 * 向指定 alias 串口写若干字节
 * 主要用于按键命令通路：'c' 进 capture / 'r' 回识别
 */
export async function sendSerialBytes(
  alias: string,
  bytes: number[]
): Promise<void> {
  await invoke("serial_write_byte", { alias, bytes });
}

/** 便捷：发送单个 ASCII 字符（带 \n） */
export async function sendChar(alias: string, ch: string): Promise<void> {
  const buf: number[] = [];
  for (let i = 0; i < ch.length; i += 1) {
    buf.push(ch.charCodeAt(i) & 0xff);
  }
  buf.push(0x0a); // '\n'
  await sendSerialBytes(alias, buf);
}

export async function getDevices(): Promise<DeviceMeta[]> {
  return invoke<DeviceMeta[]>("get_devices");
}

export async function getFps(): Promise<FpsSnapshot[]> {
  return invoke<FpsSnapshot[]>("get_fps");
}

// ---------------- Day 2: 打标 + 会话 ----------------

export async function setLabel(label: number): Promise<void> {
  await invoke("set_label", { label });
}

export async function getLabel(): Promise<number> {
  return invoke<number>("get_label");
}

export async function getSessionInfo(): Promise<SessionInfo> {
  return invoke<SessionInfo>("get_session_info");
}

export async function startSession(): Promise<string> {
  return invoke<string>("start_session");
}

export async function pauseSession(): Promise<void> {
  await invoke("pause_session");
}

export async function resumeSession(): Promise<void> {
  await invoke("resume_session");
}

export async function stopSession(): Promise<SessionSummary> {
  return invoke<SessionSummary>("stop_session");
}

// ---------------- Day 3: 流水线 + EI 上传 + 设置 ----------------

export async function getOutRoot(): Promise<string> {
  return invoke<string>("get_out_root");
}

/**
 * 修改会话输出根目录（后端会持久化到 <app_data>/app_config.json，下次启动自动恢复）。
 *
 * 录制中调用不会报错，但仅对下一次 startSession 生效。
 */
export async function setOutRoot(path: string): Promise<void> {
  await invoke("set_out_root", { path });
}

/**
 * 获取打包在发布包里的 build_dataset.py 默认路径。
 * 前端「流水线设置」首次加载时调用。
 */
export async function getDefaultScriptPath(): Promise<string> {
  return invoke<string>("get_default_script_path");
}

export async function listSessions(outRoot: string): Promise<SessionEntry[]> {
  return invoke<SessionEntry[]>("list_sessions", { outRoot });
}

/**
 * 删除 out_root 下指定的 session 目录（rm -rf）。
 *
 * 后端会严格校验：session_id 必须以 session_ 开头且不含路径分隔符；
 * 路径规范后必须仍在 out_root 下，以防反向逸出。
 */
export async function deleteSession(
  outRoot: string,
  sessionId: string
): Promise<void> {
  await invoke("delete_session", { outRoot, sessionId });
}

export async function runBuildDataset(
  args: BuildDatasetArgs
): Promise<ProcessOutcome> {
  return invoke<ProcessOutcome>("run_build_dataset", { args });
}

export async function uploadToEi(datasetRoot: string): Promise<UploadOutcome> {
  return invoke<UploadOutcome>("upload_to_ei", { datasetRoot });
}

export async function runPipeline(
  buildArgs: BuildDatasetArgs,
  datasetRoot: string
): Promise<PipelineOutcome> {
  return invoke<PipelineOutcome>("run_pipeline", { buildArgs, datasetRoot });
}

export async function setEiKey(key: string): Promise<void> {
  await invoke("set_ei_key", { key });
}

export async function hasEiKey(): Promise<boolean> {
  return invoke<boolean>("has_ei_key");
}

export async function deleteEiKey(): Promise<void> {
  await invoke("delete_ei_key");
}

// ---------------- Day 6: 校准 ----------------

/**
 * 触发一次完整校准流程。
 *
 * Rust 端实现：先发 'r\n'（确保板子在 RECOGNIZE / IDLE 模式），
 * 等 50ms 让 firmware 处理完 'r'，再发 'k\n' 启动 runCalibrationFlow。
 *
 * 校准期间板子 ~21s 阻塞，无法响应任何串口命令；进度通过
 * 'calibration-event' 事件流式回到前端。
 */
export async function startCalibration(alias: string): Promise<void> {
  await invoke("start_calibration", { alias });
}

/**
 * 请求 firmware 打印当前 NVS 校准内容。
 * 异步：firmware 收到后会回 [CAL_INFO] marker，前端通过 'calibration-event' 拿到。
 */
export async function readCalibrationState(alias: string): Promise<void> {
  await invoke("read_calibration_state", { alias });
}

/**
 * 清除 NVS 中的校准数据（回退到默认值）。
 * 清除后 firmware 会再次打印 [CAL_INFO] 反馈最新状态。
 */
export async function clearCalibration(alias: string): Promise<void> {
  await invoke("clear_calibration", { alias });
}

// ---------------- Day 6: 设备配置（角色 / MAC / WiFi）----------------
//
// 以下 6 个 command 全部触发 firmware 写 NVS + 3s 自动重启；
// 重启后 firmware 重新打印 [CFG_INFO] 行，前端通过 'calibration-event' Cfg 收到。
// UI 调用前应弹出二次确认提示"设备将自动重启"。

/** 切换设备角色（master / slave）。设备 3s 后重启。 */
export async function setDeviceRole(
  alias: string,
  role: "master" | "slave",
): Promise<void> {
  await invoke("set_device_role", { alias, role });
}

/** 设置 ESP-NOW 对端 MAC（"AA:BB:CC:DD:EE:FF"）。设备 3s 后重启。 */
export async function setPeerMac(alias: string, mac: string): Promise<void> {
  await invoke("set_peer_mac", { alias, mac });
}

/** 设置 WiFi 凭据。SSID/密码不能含空格或换行。设备 3s 后重启。 */
export async function setWifi(
  alias: string,
  ssid: string,
  password: string,
): Promise<void> {
  await invoke("set_wifi", { alias, ssid, password });
}

/** 清除 NVS 中的 WiFi 凭据。设备 3s 后重启。 */
export async function clearDeviceWifi(alias: string): Promise<void> {
  await invoke("clear_device_wifi", { alias });
}

/** 清除 NVS 中的角色 / 对端 MAC（恢复编译默认）。设备 3s 后重启。 */
export async function clearNvsRole(alias: string): Promise<void> {
  await invoke("clear_nvs_role", { alias });
}

/**
 * 请求 firmware 打印 [CFG_INFO]。
 * 异步：firmware 收到后会回 [CFG_INFO] marker，前端通过 'calibration-event' Cfg 拿到。
 */
export async function readDeviceInfo(alias: string): Promise<void> {
  await invoke("read_device_info", { alias });
}

