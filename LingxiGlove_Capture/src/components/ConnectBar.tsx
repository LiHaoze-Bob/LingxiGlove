/**
 * ConnectBar — 单设备连接栏
 *
 * Day 1 用户故事：
 *   1) 点「重扫」获取最新串口列表（macOS /dev/cu.*）
 *   2) 下拉选 port + 输入波特率
 *   3) 点「连接」启动后端 SerialTask
 *   4) 已连接时按钮变「断开」
 */
import { useEffect, useState } from "react";
import {
  connectDevice,
  disconnectDevice,
} from "../api";
import type {
  DeviceStatus,
  SerialPortInfo,
} from "../types";
import { COMMON_BAUDS, DEFAULT_BAUD } from "../types";

interface Props {
  alias: string;
  /** UI 显示名（默认 alias.toUpperCase()，Day 5 用 "MASTER" 覆盖 LEFT） */
  displayName?: string;
  status: DeviceStatus;
  ports: SerialPortInfo[];
  onPortsRefresh: () => Promise<void>;
  onChanged: () => Promise<void>;
}

export function ConnectBar({
  alias,
  displayName,
  status,
  ports,
  onPortsRefresh,
  onChanged,
}: Props) {
  const [port, setPort] = useState<string>("");
  const [baud, setBaud] = useState<number>(DEFAULT_BAUD);
  const [busy, setBusy] = useState(false);
  const [err, setErr] = useState<string | null>(null);

  // 第一次有 ports 时自动选第一个，方便测试
  useEffect(() => {
    if (!port && ports.length > 0) setPort(ports[0].port_name);
  }, [ports, port]);

  const isConnected = status === "connected";
  const isError = status === "error";

  async function handleClick() {
    setBusy(true);
    setErr(null);
    try {
      if (isConnected) {
        await disconnectDevice(alias);
      } else {
        if (!port) throw new Error("请先选择串口");
        await connectDevice(alias, port, baud);
      }
      await onChanged();
    } catch (e) {
      setErr(String(e));
    } finally {
      setBusy(false);
    }
  }

  return (
    <div className="connbar">
      <div className="connbar__label">
        <span className={`dot dot--${status}`} />
        {displayName ?? alias.toUpperCase()}
      </div>

      <select
        value={port}
        onChange={(e) => setPort(e.target.value)}
        disabled={isConnected || busy}
        title={port || "选择串口"}
      >
        <option value="">— 选择串口 —</option>
        {ports.map((p) => (
          <option key={p.port_name} value={p.port_name}>
            {p.port_name}
            {p.description ? `  (${p.description})` : ""}
          </option>
        ))}
      </select>

      <select
        value={baud}
        onChange={(e) => setBaud(Number(e.target.value))}
        disabled={isConnected || busy}
      >
        {COMMON_BAUDS.map((b) => (
          <option key={b} value={b}>
            {b}
          </option>
        ))}
      </select>

      <button
        type="button"
        className="btn btn--ghost"
        disabled={busy}
        onClick={() => onPortsRefresh()}
        title="重新枚举系统串口"
      >
        重扫
      </button>

      <button
        type="button"
        className={isConnected ? "btn btn--danger" : "btn"}
        disabled={busy || (!isConnected && !port)}
        onClick={handleClick}
      >
        {busy ? "…" : isConnected ? "断开" : "连接"}
      </button>

      {(err || isError) && (
        <div
          style={{
            gridColumn: "1 / -1",
            fontSize: 12,
            color: "#fca5a5",
          }}
        >
          {err || "设备异常，已停止读取"}
        </div>
      )}
    </div>
  );
}
