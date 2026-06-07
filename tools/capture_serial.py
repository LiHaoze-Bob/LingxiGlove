#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
capture_serial.py — LingxiGlove 串口数据采集器

用途：
  端侧进入 MODE_CAPTURE（串口键入 'c'）后，每行打印一条 CSV：
    timestamp_ms,ax,ay,az,gx,gy,gz,pitch,roll,flex0..flex4,label
  本脚本通过 pyserial 抓取串口流量并按会话写入：
    LingxiGlove/output/capture/session_<YYYYmmdd_HHMMSS>/raw.csv

  - 自动识别 header 行（包含 "timestamp_ms"），仅保留首份 header
  - 跳过非数据行（[ 开头的端侧日志 / 空行 / 校准提示等）
  - Ctrl+C 结束后打印各 label 的行数统计

CLI 示例：
    python tools/capture_serial.py                   # 自动发现 cu.usbmodem* 端口
    python tools/capture_serial.py --port /dev/cu.usbmodem14101
    python tools/capture_serial.py --baud 115200 --out output/capture

依赖：
    pip install pyserial

注意：
  - 临时/会话文件统一落到工作区的 output 目录（项目规范）
  - 端侧需先进入 MODE_CAPTURE 并使用数字键打 label，否则 label 列均为 -1
"""

from __future__ import annotations

import argparse
import datetime as _dt
import glob
import os
import signal
import sys
import time
from collections import Counter
from pathlib import Path

try:
    import serial  # type: ignore
except ImportError:
    print("[ERROR] 缺少依赖 pyserial，请运行: pip install pyserial", file=sys.stderr)
    sys.exit(2)


# ---------------- 路径 / 默认值 ----------------

# 脚本位于 LingxiGlove/tools/ 下；output 目录约定在 LingxiGlove/output/
SCRIPT_DIR = Path(__file__).resolve().parent
DEFAULT_OUT_ROOT = SCRIPT_DIR.parent / "output" / "capture"

DEFAULT_BAUD = 115200
HEADER_KEY = "timestamp_ms"  # 用于识别 CSV header 的关键字（区别于日志行）


# ---------------- 端口发现 ----------------

def auto_detect_port() -> str | None:
    """在 macOS / Linux / Windows 下尽力发现 Arduino Nano ESP32 的串口设备路径。"""
    candidates: list[str] = []
    candidates += sorted(glob.glob("/dev/cu.usbmodem*"))   # macOS
    candidates += sorted(glob.glob("/dev/cu.usbserial-*"))
    candidates += sorted(glob.glob("/dev/ttyACM*"))         # Linux
    candidates += sorted(glob.glob("/dev/ttyUSB*"))
    # Windows 用户请显式 --port COMx
    return candidates[0] if candidates else None


# ---------------- 主流程 ----------------

class CaptureSession:
    """一次采集会话：打开串口 -> 写 CSV -> 收尾统计。"""

    def __init__(self, port: str, baud: int, out_root: Path):
        self.port = port
        self.baud = baud
        self.out_root = out_root
        self.session_dir: Path | None = None
        self.csv_path: Path | None = None
        self.header_written = False
        self.line_count = 0  # 数据行数（不含 header）
        self.label_counter: Counter[str] = Counter()
        self._stopping = False

    # ------- 打印小工具 -------
    @staticmethod
    def _eprint(msg: str) -> None:
        print(msg, file=sys.stderr, flush=True)

    # ------- 会话管理 -------
    def _make_session_dir(self) -> Path:
        ts = _dt.datetime.now().strftime("session_%Y%m%d_%H%M%S")
        path = self.out_root / ts
        path.mkdir(parents=True, exist_ok=True)
        return path

    def _is_data_line(self, line: str) -> bool:
        """数据行判定：以数字开头 + 含至少 9 个英文逗号（覆盖 9 列固定 + 5 flex + label = 15 列）。

        端侧日志一律 '[' 开头；ANSI / 空行直接丢弃。
        """
        if not line:
            return False
        if line.startswith("["):
            return False
        # 必须以数字开头（timestamp_ms 是 unsigned long）
        c0 = line[0]
        if not (c0.isdigit()):
            return False
        # 至少含 9 个逗号（兼容未来扩列；少于 9 几乎必为脏行）
        return line.count(",") >= 9

    def _is_header_line(self, line: str) -> bool:
        return line.startswith(HEADER_KEY)

    def _count_label_from_row(self, line: str) -> None:
        # label 是行末最后一个字段
        last_comma = line.rfind(",")
        if last_comma < 0:
            return
        label = line[last_comma + 1 :].strip()
        if label:
            self.label_counter[label] += 1

    def _print_progress(self) -> None:
        if self.line_count == 0 or self.line_count % 100 != 0:
            return
        # 简短进度：行数 + 当前 label 分布
        parts = [f"{lab}:{cnt}" for lab, cnt in sorted(self.label_counter.items())]
        self._eprint(f"[capture] rows={self.line_count}  " + "  ".join(parts))

    # ------- 主循环 -------
    def run(self) -> int:
        self.session_dir = self._make_session_dir()
        self.csv_path = self.session_dir / "raw.csv"
        self._eprint(f"[capture] 会话目录: {self.session_dir}")
        self._eprint(f"[capture] 打开串口: {self.port} @ {self.baud}")

        try:
            ser = serial.Serial(self.port, self.baud, timeout=1.0)
        except Exception as e:
            self._eprint(f"[ERROR] 串口打开失败: {e}")
            return 1

        # 给端侧上电后稳定一点时间（不强制重置）
        time.sleep(0.2)

        with self.csv_path.open("w", encoding="utf-8", newline="") as fp:
            self._eprint("[capture] 已开始抓取，端侧请按 'c' 进入采集模式；按数字键 0/1/2 切换 label")
            self._eprint("[capture] Ctrl+C 结束采集")

            try:
                while not self._stopping:
                    raw = ser.readline()
                    if not raw:
                        continue
                    try:
                        line = raw.decode("utf-8", errors="replace").rstrip("\r\n")
                    except Exception:
                        continue

                    # header
                    if self._is_header_line(line):
                        if not self.header_written:
                            fp.write(line + "\n")
                            fp.flush()
                            self.header_written = True
                            self._eprint(f"[capture] header: {line}")
                        # 重复 header 忽略（端侧重新进入采集模式可能再打一次）
                        continue

                    if not self._is_data_line(line):
                        # 直接转发到 stderr，方便用户看到端侧日志
                        if line.strip():
                            self._eprint(line)
                        continue

                    if not self.header_written:
                        # 端侧已经在采集中且我们晚到了；写一个默认 header，避免数据无列名
                        default_header = (
                            "timestamp_ms,ax,ay,az,gx,gy,gz,pitch,roll,"
                            "flex0,flex1,flex2,flex3,flex4,label"
                        )
                        fp.write(default_header + "\n")
                        fp.flush()
                        self.header_written = True
                        self._eprint(f"[capture] header(默认补写): {default_header}")

                    fp.write(line + "\n")
                    self.line_count += 1
                    self._count_label_from_row(line)
                    if self.line_count % 50 == 0:
                        fp.flush()
                    self._print_progress()
            except KeyboardInterrupt:
                self._stopping = True
            finally:
                fp.flush()
                ser.close()

        self._summary()
        return 0

    def _summary(self) -> None:
        if self.csv_path is None:
            return
        self._eprint("\n[capture] ====== 会话汇总 ======")
        self._eprint(f"  CSV 文件 : {self.csv_path}")
        self._eprint(f"  数据行数 : {self.line_count}")
        if not self.label_counter:
            self._eprint("  label 分布: (空)")
            return
        # 把 label 数字翻译成名称（与端侧 CAPTURE_LABEL_NAMES 对齐）
        label_names = {"-1": "unlabeled", "0": "straight", "1": "half", "2": "full"}
        for lab, cnt in sorted(self.label_counter.items(), key=lambda kv: kv[0]):
            name = label_names.get(lab, "?")
            self._eprint(f"  label={lab:>3} ({name:>10}): {cnt} 行")


# ---------------- CLI ----------------

def parse_args(argv: list[str]) -> argparse.Namespace:
    p = argparse.ArgumentParser(
        description="LingxiGlove 串口采集器（端侧 MODE_CAPTURE -> CSV）",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    p.add_argument("--port", default="auto",
                   help="串口设备路径，'auto' 自动发现 cu.usbmodem*")
    p.add_argument("--baud", type=int, default=DEFAULT_BAUD, help="波特率")
    p.add_argument("--out", default=str(DEFAULT_OUT_ROOT),
                   help="会话输出根目录，默认 LingxiGlove/output/capture")
    return p.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)

    port = args.port
    if port == "auto":
        detected = auto_detect_port()
        if not detected:
            print("[ERROR] 未发现串口设备，请显式指定 --port /dev/cu.usbmodemXXXX",
                  file=sys.stderr)
            return 2
        port = detected

    out_root = Path(args.out).expanduser().resolve()
    out_root.mkdir(parents=True, exist_ok=True)

    sess = CaptureSession(port=port, baud=args.baud, out_root=out_root)

    # SIGTERM 也能优雅收尾（macOS 偶尔从 IDE 直接 kill）
    def _sig_handler(signum, frame):  # noqa: ARG001
        sess._stopping = True

    signal.signal(signal.SIGTERM, _sig_handler)
    return sess.run()


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
