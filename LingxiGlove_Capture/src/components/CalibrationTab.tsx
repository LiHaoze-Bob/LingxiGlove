/**
 * CalibrationTab — 设备校准 Tab
 *
 * 职责：
 *  - 维护两个独立 alias（CAL_LEFT / CAL_RIGHT）的 DeviceCalibrationCard
 *  - 共享串口列表（从 list_ports 拿）+ 设备元信息（从 get_devices 拿）
 *  - 订阅 'serial-role' 事件：路由到对应 alias 的 card
 *  - 订阅 'calibration-event' 事件：拆分 progress / info 路由到对应 alias 的 card
 *
 * 切到本 Tab 不影响 capture tab 的连接，反之亦然——alias 不同则后端
 * 的 SerialTask 完全独立。
 */
import { useCallback, useEffect, useState } from "react";
import { listen } from "@tauri-apps/api/event";
import { DeviceCalibrationCard } from "./DeviceCalibrationCard";
import { getDevices, listPorts } from "../api";
import { useCaptureStore } from "../store";
import {
  CAL_LEFT_ALIAS,
  CAL_RIGHT_ALIAS,
  type CalEvent,
  type DeviceMeta,
  type SerialPortInfo,
  type SerialRoleEvent,
} from "../types";

export function CalibrationTab() {
  const [ports, setPorts] = useState<SerialPortInfo[]>([]);
  const [devices, setDevices] = useState<DeviceMeta[]>([]);

  const setCalCardRole = useCaptureStore((s) => s.setCalCardRole);
  const setCalCardInfo = useCaptureStore((s) => s.setCalCardInfo);
  const setCalCardCfg = useCaptureStore((s) => s.setCalCardCfg);
  const applyCalProgress = useCaptureStore((s) => s.applyCalProgress);

  // 双板互填用：对端的 self_mac 即本端应填入的 peer_mac
  const leftSelfMac = useCaptureStore(
    (s) => s.calibrationCards[CAL_LEFT_ALIAS]?.cfgInfo?.self_mac ?? null
  );
  const rightSelfMac = useCaptureStore(
    (s) => s.calibrationCards[CAL_RIGHT_ALIAS]?.cfgInfo?.self_mac ?? null
  );

  const refreshPorts = useCallback(async () => {
    try {
      setPorts(await listPorts());
    } catch (e) {
      console.error("listPorts failed:", e);
    }
  }, []);

  const refreshDevices = useCallback(async () => {
    try {
      setDevices(await getDevices());
    } catch (e) {
      console.error("getDevices failed:", e);
    }
  }, []);

  useEffect(() => {
    refreshPorts();
    refreshDevices();

    // 监听 serial-role：仅处理 CAL_* alias
    const unsubRolePromise = listen<SerialRoleEvent>(
      "serial-role",
      (event) => {
        const ev = event.payload;
        if (ev.alias !== CAL_LEFT_ALIAS && ev.alias !== CAL_RIGHT_ALIAS) return;
        setCalCardRole(ev.alias, ev.role);
      }
    );

    // 监听 calibration-event：路由到对应 alias 的 card
    const unsubCalPromise = listen<CalEvent>(
      "calibration-event",
      (event) => {
        const ev = event.payload;
        if (ev.alias !== CAL_LEFT_ALIAS && ev.alias !== CAL_RIGHT_ALIAS) return;
        if (ev.kind === "progress") {
          applyCalProgress(ev.alias, {
            stage: ev.stage,
            phase: ev.phase,
            remain: ev.remain,
            ok: ev.ok,
            reason: ev.reason,
            flags: ev.flags,
          });
        } else if (ev.kind === "info") {
          setCalCardInfo(ev.alias, {
            flags: ev.flags,
            accel_bias: ev.accel_bias,
            gyro_bias: ev.gyro_bias,
            flex_min: ev.flex_min,
            flex_max: ev.flex_max,
          });
        } else if (ev.kind === "cfg") {
          setCalCardCfg(ev.alias, {
            role: ev.role,
            self_mac: ev.self_mac,
            peer_mac: ev.peer_mac,
            ssid: ev.ssid,
            wifi_pwd: ev.wifi_pwd,
            wifi_connected: ev.wifi_connected,
            ip: ev.ip,
            rssi: ev.rssi,
            mode: ev.mode,
          });
        }
      }
    );

    // 1Hz 刷新设备状态（便于 ConnectBar 状态显示）
    const timer = setInterval(() => {
      refreshDevices();
    }, 1000);

    return () => {
      clearInterval(timer);
      unsubRolePromise.then((u) => u());
      unsubCalPromise.then((u) => u());
    };
  }, [
    refreshPorts,
    refreshDevices,
    setCalCardRole,
    setCalCardInfo,
    setCalCardCfg,
    applyCalProgress,
  ]);

  return (
    <section className="section cal-tab">
      <div className="cal-tab__hint">
        <strong>双板独立校准 &amp; 配置。</strong>
        本 Tab 使用独立的 alias <code>CAL_LEFT</code> / <code>CAL_RIGHT</code>，
        与数据采集 Tab（<code>left</code> / <code>right</code>）互不影响。
        <br />
        校准（IMU 零偏 + Flex 量程）/ 角色 / 对端 MAC / WiFi 均写入板载 NVS。
        <strong> 修改角色 / MAC / WiFi 时设备会自动重启 (~3s)。</strong>
      </div>

      <div className="cal-tab__cards">
        <DeviceCalibrationCard
          alias={CAL_LEFT_ALIAS}
          displayName="左手"
          ports={ports}
          onPortsRefresh={refreshPorts}
          devices={devices}
          onDevicesChanged={refreshDevices}
          peerSelfMac={rightSelfMac}
        />
        <DeviceCalibrationCard
          alias={CAL_RIGHT_ALIAS}
          displayName="右手"
          ports={ports}
          onPortsRefresh={refreshPorts}
          devices={devices}
          onDevicesChanged={refreshDevices}
          peerSelfMac={leftSelfMac}
        />
      </div>
    </section>
  );
}
