/**
 * RealtimePlot — 单设备实时多通道折线图
 *
 * 性能策略（与 cfcd4a1f 记忆里 uPlot 避坑清单一致）：
 * - useRef 维护 ring buffer，**不进 React state**（双设备 40Hz 进 state 必卡）
 * - listen("frame") 在组件内部直接 push ring，过滤自己 alias 的帧
 * - rAF 60fps tick → uPlot.setData(整组 array)（uPlot 推荐用法）
 * - cursor.show=false / legend.live=false / y 轴 auto:false 全部关闭
 * - 只有一个 uPlot 实例，多 series；resize 监听 devicePixelRatio
 *
 * 默认绘 5 路 flex（阶段 1 主路径），通道可由 prop 覆盖。
 */
import { memo, useEffect, useMemo, useRef } from "react";
import uPlot from "uplot";
import "uplot/dist/uPlot.min.css";
import { listen } from "@tauri-apps/api/event";
import type { ChannelName, Frame } from "../types";
import { CHANNEL_NAMES, DEFAULT_PLOT_CHANNELS } from "../types";

interface RealtimePlotProps {
  alias: string;
  /** UI 标题（默认 alias.toUpperCase()，Day 5 用 "Master"/"Slave" 覆盖） */
  displayName?: string;
  /** 要绘的通道（按通道名）；不传走 DEFAULT_PLOT_CHANNELS */
  channels?: ChannelName[];
  /** 显示窗口宽度（点数，frame_count，默认 100 ≈ 5s @ 20fps） */
  windowSize?: number;
  /** y 轴范围，固定不自适应（避免抖动） */
  yMin?: number;
  yMax?: number;
  /** 容器高度（px） */
  height?: number;
}

/** 每通道一种区分度高的颜色（深色背景下） */
const SERIES_COLORS = [
  "#38bdf8", // sky-400
  "#a78bfa", // violet-400
  "#f472b6", // pink-400
  "#fbbf24", // amber-400
  "#34d399", // emerald-400
  "#f87171", // red-400
  "#60a5fa", // blue-400
  "#facc15", // yellow-400
  "#4ade80", // green-400
  "#fb923c", // orange-400
];

function channelToValueIndex(ch: ChannelName): number {
  // CHANNEL_NAMES 对应 frame.values（不含 timestamp_ms 列）
  return CHANNEL_NAMES.indexOf(ch);
}

function RealtimePlotImpl(props: RealtimePlotProps) {
  const {
    alias,
    displayName,
    channels = DEFAULT_PLOT_CHANNELS,
    windowSize = 100,
    yMin = 0,
    yMax = 4096,
    height = 220,
  } = props;

  const containerRef = useRef<HTMLDivElement | null>(null);
  const plotRef = useRef<uPlot | null>(null);

  // ring buffer：xs[i] = i * 50ms（不依赖 dev_ts，纯递增刻度）
  // ys[chIdx][i] = 第 chIdx 通道在该位置的值
  const xsRef = useRef<number[]>([]);
  const ysRef = useRef<number[][]>([]);
  const seqRef = useRef<number>(0); // 全局序号 → x 轴

  // 通道值索引列表（如 [8,9,10,11,12] 表示 flex0..flex4 在 frame.values 里的位置）
  const chIdx = useMemo(() => channels.map(channelToValueIndex), [channels]);

  // 初始化空 ring
  useEffect(() => {
    xsRef.current = Array.from({ length: windowSize }, (_, i) => i);
    ysRef.current = chIdx.map(() => Array(windowSize).fill(NaN));
    seqRef.current = windowSize;
  }, [windowSize, chIdx]);

  // 初始化 uPlot 实例
  useEffect(() => {
    if (!containerRef.current) return;

    const series: uPlot.Series[] = [
      {}, // x-axis
      ...channels.map((name, i) => ({
        label: name,
        stroke: SERIES_COLORS[i % SERIES_COLORS.length],
        width: 1.2,
        points: { show: false },
        spanGaps: true,
      })),
    ];

    const opts: uPlot.Options = {
      width: containerRef.current.clientWidth || 600,
      height,
      cursor: { show: false, x: false, y: false, drag: { x: false, y: false } },
      legend: { show: true, live: false },
      select: { show: false, left: 0, top: 0, width: 0, height: 0 },
      scales: {
        x: { time: false, auto: false },
        y: { auto: false, range: () => [yMin, yMax] },
      },
      axes: [
        { stroke: "#94a3b8", grid: { stroke: "rgba(148,163,184,0.15)" } },
        {
          stroke: "#94a3b8",
          grid: { stroke: "rgba(148,163,184,0.15)" },
          values: (_u, ticks) => ticks.map((v) => Math.round(v).toString()),
        },
      ],
      series,
    };

    const data: uPlot.AlignedData = [
      xsRef.current,
      ...ysRef.current,
    ] as uPlot.AlignedData;
    const u = new uPlot(opts, data, containerRef.current);
    plotRef.current = u;

    // 容器尺寸变化（窗口 resize）
    const resizeObs = new ResizeObserver((entries) => {
      const e = entries[0];
      if (!e || !plotRef.current) return;
      plotRef.current.setSize({ width: Math.floor(e.contentRect.width), height });
    });
    resizeObs.observe(containerRef.current);

    return () => {
      resizeObs.disconnect();
      u.destroy();
      plotRef.current = null;
    };
  }, [channels, height, yMin, yMax]);

  // 监听 frame 事件 + rAF tick
  useEffect(() => {
    let unlisten: (() => void) | null = null;
    let rafId = 0;
    let dirty = false;

    listen<Frame>("frame", (event) => {
      const f = event.payload;
      if (f.dev_alias !== alias) return;
      // ring buffer push：x 自增序号；ys 取对应通道值
      const xs = xsRef.current;
      const ys = ysRef.current;
      // 每个 ring 都 shift+push（O(n)，n=100，可接受）
      seqRef.current += 1;
      xs.shift();
      xs.push(seqRef.current);
      for (let i = 0; i < chIdx.length; i++) {
        const idx = chIdx[i];
        const v =
          idx >= 0 && idx < f.values.length ? f.values[idx] : NaN;
        ys[i].shift();
        ys[i].push(v);
      }
      dirty = true;
    }).then((u) => {
      unlisten = u;
    });

    const tick = () => {
      if (dirty && plotRef.current) {
        plotRef.current.setData(
          [xsRef.current, ...ysRef.current] as uPlot.AlignedData
        );
        dirty = false;
      }
      rafId = requestAnimationFrame(tick);
    };
    rafId = requestAnimationFrame(tick);

    return () => {
      cancelAnimationFrame(rafId);
      if (unlisten) unlisten();
    };
  }, [alias, chIdx]);

  return (
    <div className="plot">
      <div className="plot__title">
        <span className="plot__alias">{displayName ?? alias.toUpperCase()}</span>
        <span className="plot__hint">
          {channels.join(" · ")} · y∈[{yMin}, {yMax}]
        </span>
      </div>
      <div ref={containerRef} className="plot__canvas" style={{ height }} />
    </div>
  );
}

export const RealtimePlot = memo(RealtimePlotImpl);
