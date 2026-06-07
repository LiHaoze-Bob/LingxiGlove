/**
 * CaptureTab — 数据采集 Tab（原 App 主体内容）
 *
 * Day 6：从 App.tsx 抽离，与 CalibrationTab 通过顶层 Tab 切换。
 *
 * 布局：
 *   ConnectBar × 1（仅 master）
 *   SessionToolbar
 *   LabelHUD
 *   RealtimePlot × 2
 *   StatsCard × 2
 *   PipelinePanel
 *
 * 监听 frame / serial-role 事件，1Hz 拉取 fps + session info。
 */
import { useCallback, useEffect, useState } from "react";
import { listen } from "@tauri-apps/api/event";
import { ConnectBar } from "./ConnectBar";
import { StatsCard } from "./StatsCard";
import { RealtimePlot } from "./RealtimePlot";
import { LabelHUD } from "./LabelHUD";
import { SessionToolbar } from "./SessionToolbar";
import { PipelinePanel } from "./PipelinePanel";
import { disconnectDevice, getDevices, getFps, listPorts } from "../api";
import { useCaptureStore } from "../store";
import type {
  DeviceMeta,
  FpsSnapshot,
  Frame,
  SerialPortInfo,
  SerialRoleEvent,
} from "../types";

/** Day 5：采集 tab 内 master alias 固定为 "left"（与 calibration tab 的 CAL_LEFT 隔离） */
const MASTER_ALIAS = "left";
/** Slave 数据由 master ESP-NOW 转发并在后端拆为 alias="right" 的 frame */
const SLAVE_ALIAS = "right";

interface RoleToast {
  kind: "ok" | "warn" | "err";
  msg: string;
}

interface Props {
  /** 透传 footer 用：当前已连接 master 数 + 总 fps */
  onMetrics?: (m: { masterConnected: number; totalFps: number; portCount: number }) => void;
}

export function CaptureTab({ onMetrics }: Props) {
  const [ports, setPorts] = useState<SerialPortInfo[]>([]);
  const [devices, setDevices] = useState<DeviceMeta[]>([]);
  const [fps, setFps] = useState<FpsSnapshot[]>([]);
  const [lastFrames, setLastFrames] = useState<Record<string, Frame>>({});
  const [roleToast, setRoleToast] = useState<RoleToast | null>(null);

  const refreshSessionInfo = useCaptureStore((s) => s.refreshSessionInfo);
  const syncLabelFromBackend = useCaptureStore((s) => s.syncLabelFromBackend);
  const setMasterRole = useCaptureStore((s) => s.setMasterRole);
  const setCaptureFlow = useCaptureStore((s) => s.setCaptureFlow);
  const captureFlow = useCaptureStore((s) => s.captureFlow);

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

  // 启动初始化
  useEffect(() => {
    refreshPorts();
    refreshDevices();
    syncLabelFromBackend();
    refreshSessionInfo();

    // 监听 frame：仅做 last raw 缓存（StatsCard 用），uPlot 自己监听
    const lastFrameMap = new Map<string, Frame>();
    let dirty = false;
    const unsubFramePromise = listen<Frame>("frame", (event) => {
      const f = event.payload;
      // 校准 tab 用了 CAL_LEFT/CAL_RIGHT alias，不会污染这里的 left/right
      if (f.dev_alias !== MASTER_ALIAS && f.dev_alias !== SLAVE_ALIAS) return;
      lastFrameMap.set(f.dev_alias, f);
      dirty = true;
    });

    // 监听 serial-role：仅处理 master alias
    const unsubRolePromise = listen<SerialRoleEvent>(
      "serial-role",
      async (event) => {
        const ev = event.payload;
        if (ev.alias !== MASTER_ALIAS) return;
        setMasterRole(ev.role);
        if (ev.role === "master") {
          setRoleToast({
            kind: "ok",
            msg: "✅ 已识别 MASTER 角色，可以开始采集",
          });
          setCaptureFlow("IDLE");
        } else if (ev.role === "slave") {
          setRoleToast({
            kind: "err",
            msg: "⚠️ 当前连接的是 SLAVE 板，请改连 MASTER 板（或先 'role master' 切换并重启）",
          });
          try {
            await disconnectDevice(MASTER_ALIAS);
          } catch (e) {
            console.warn("auto disconnect after slave detected failed:", e);
          }
          setCaptureFlow("DISCONNECTED");
          await refreshDevices();
        } else {
          setRoleToast({
            kind: "warn",
            msg: "⚠️ 未识别到角色 banner，可能 firmware 较旧。已默认进入 IDLE",
          });
          setCaptureFlow("IDLE");
        }
        setTimeout(() => setRoleToast(null), 5000);
      }
    );

    // 1Hz 刷新 fps + last raw + session info
    const timer = setInterval(async () => {
      try {
        const snap = await getFps();
        setFps(snap);
        if (dirty) {
          const obj: Record<string, Frame> = {};
          lastFrameMap.forEach((v, k) => (obj[k] = v));
          setLastFrames(obj);
          dirty = false;
        }
        await refreshSessionInfo();
      } catch (e) {
        console.error("1Hz tick failed:", e);
      }
    }, 1000);

    return () => {
      clearInterval(timer);
      unsubFramePromise.then((unsub) => unsub());
      unsubRolePromise.then((unsub) => unsub());
    };
  }, [
    refreshPorts,
    refreshDevices,
    refreshSessionInfo,
    syncLabelFromBackend,
    setMasterRole,
    setCaptureFlow,
  ]);

  // master 设备 connect/disconnect 状态变化 → 同步 captureFlow
  useEffect(() => {
    const masterMeta = devices.find((d) => d.alias === MASTER_ALIAS);
    const isConnected = masterMeta?.status === "connected";
    if (!isConnected) {
      if (captureFlow !== "DISCONNECTED") {
        setCaptureFlow("DISCONNECTED");
        setMasterRole(null);
      }
    } else if (captureFlow === "DISCONNECTED") {
      setCaptureFlow("HANDSHAKING");
    }
  }, [devices, captureFlow, setCaptureFlow, setMasterRole]);

  // 上报 footer 指标
  useEffect(() => {
    if (!onMetrics) return;
    const masterMeta = devices.find((d) => d.alias === MASTER_ALIAS);
    const masterConnected = masterMeta?.status === "connected" ? 1 : 0;
    const totalFps = fps
      .filter((f) => f.alias === MASTER_ALIAS || f.alias === SLAVE_ALIAS)
      .reduce((a, b) => a + b.fps, 0);
    onMetrics({ masterConnected, totalFps, portCount: ports.length });
  }, [devices, fps, ports, onMetrics]);

  const metaOf = (alias: string) => devices.find((d) => d.alias === alias);
  const fpsOf = (alias: string) => fps.find((f) => f.alias === alias);

  return (
    <>
      {roleToast && (
        <div
          className={`role-toast role-toast--${roleToast.kind}`}
          role="alert"
        >
          {roleToast.msg}
          <button
            type="button"
            className="role-toast__close"
            onClick={() => setRoleToast(null)}
          >
            ×
          </button>
        </div>
      )}

      {/* 连接栏（仅 1 个 Master） */}
      <section className="section">
        <ConnectBar
          alias={MASTER_ALIAS}
          displayName="MASTER"
          status={metaOf(MASTER_ALIAS)?.status ?? "idle"}
          ports={ports}
          onPortsRefresh={refreshPorts}
          onChanged={refreshDevices}
        />
      </section>

      {/* 录制工具条 */}
      <section className="section">
        <SessionToolbar />
      </section>

      {/* 当前 label HUD */}
      <section className="section">
        <LabelHUD />
      </section>

      {/* 双面板实时绘图：Master / Slave */}
      <section className="section section--plots">
        <RealtimePlot alias={MASTER_ALIAS} displayName="Master" />
        <RealtimePlot alias={SLAVE_ALIAS} displayName="Slave" />
      </section>

      {/* 诊断统计 */}
      <section className="section section--stats">
        <StatsCard
          meta={metaOf(MASTER_ALIAS)}
          fps={fpsOf(MASTER_ALIAS)}
          lastFrame={lastFrames[MASTER_ALIAS]}
          displayName="Master"
        />
        <StatsCard
          meta={metaOf(SLAVE_ALIAS)}
          fps={fpsOf(SLAVE_ALIAS)}
          lastFrame={lastFrames[SLAVE_ALIAS]}
          displayName="Slave"
        />
      </section>

      {/* Day 3 数据流水线 */}
      <section className="section">
        <PipelinePanel />
      </section>
    </>
  );
}
