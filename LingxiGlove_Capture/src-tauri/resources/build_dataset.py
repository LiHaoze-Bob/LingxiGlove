#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
build_dataset.py — LingxiGlove 数据集构建器

把 capture_serial.py 录到的会话 CSV，切窗 + 拆分 train/test，并产出两种格式：

  1) Edge Impulse Data Acquisition CSV（用于 EI Studio 上传）
     output/dataset/ei_csv/{train,test}/<label_name>.<seq>.csv
     文件内首行 header："timestamp,flex"
     文件名前缀为 label 名称，EI 上传时自动按文件名解析 label。

  2) 统一汇总 numpy（用于 eval_offline.py 复跑）
     output/dataset/X_train.npy / y_train.npy / X_test.npy / y_test.npy
     X.shape = (N, WIN_FRAMES, CHANNEL_COUNT) — 当前 CHANNEL_COUNT=1（仅食指）

切窗策略：
  - 默认窗口 20 帧 (@20Hz = 1.0s)，步长 10 帧（50% 重叠）
  - 窗口内所有行的 label 必须一致；含 label<0 (unlabeled) 直接丢弃
  - 切完按 8:2 拆 train/test（同 label 内随机打乱后切，保证类别分布）

CLI 示例：
    python tools/build_dataset.py
    python tools/build_dataset.py --in output/capture --window 20 --stride 10 \
                                  --flex-channel 1 --test-ratio 0.2

依赖：
    标准库；numpy 仅当 --emit-numpy 默认开启时需要（pip install numpy）
"""

from __future__ import annotations

import argparse
import csv
import random
import sys
from collections import Counter, defaultdict
from dataclasses import dataclass
from pathlib import Path

# numpy 是软依赖：若未安装且未关闭 --emit-numpy 则提示
try:
    import numpy as _np  # type: ignore
except ImportError:
    _np = None  # noqa: N816


SCRIPT_DIR = Path(__file__).resolve().parent
DEFAULT_IN = SCRIPT_DIR.parent / "output" / "capture"
DEFAULT_OUT = SCRIPT_DIR.parent / "output" / "dataset"

# 与端侧 config.h 的 CAPTURE_LABEL_NAMES 严格对齐
LABEL_NAMES = {0: "straight", 1: "half", 2: "full"}

# 端侧采样周期（与 SENSOR_READ_INTERVAL 一致）
DEFAULT_FRAME_PERIOD_MS = 50  # 20Hz


# ---------------- 数据结构 ----------------

@dataclass
class Window:
    """切好的一个窗口样本。"""
    label: int
    samples: list[float]  # 长度 = window_size，单通道时为 1D


@dataclass
class BimanualWindow:
    """双手联合窗口样本（26 通道：13 master + 13 slave）。"""
    label: int
    # shape: [win_size][26]，每帧一个 26 维向量
    samples: list[list[float]]


# 双手模式的通道顺序（与端侧 printBimanualCsvRow 严格对齐）
BIMANUAL_FEATURE_COLS = [
    "m_ax", "m_ay", "m_az", "m_gx", "m_gy", "m_gz", "m_pitch", "m_roll",
    "m_flex0", "m_flex1", "m_flex2", "m_flex3", "m_flex4",
    "s_ax", "s_ay", "s_az", "s_gx", "s_gy", "s_gz", "s_pitch", "s_roll",
    "s_flex0", "s_flex1", "s_flex2", "s_flex3", "s_flex4",
]
BIMANUAL_CHANNEL_COUNT = len(BIMANUAL_FEATURE_COLS)  # 26


# ---------------- CSV 读取 ----------------

def _detect_columns(header: list[str]) -> dict[str, int]:
    """根据 header 返回各关键列的索引；缺列时抛 KeyError。"""
    needed = {"label"}
    idx = {name: i for i, name in enumerate(header) if name in needed}
    if "label" not in idx:
        raise KeyError(
            f"CSV header 缺少 'label' 列；请确认端侧已经升级到带 label 列的协议。"
            f"当前 header: {header}"
        )
    return idx


def _flex_column_index(header: list[str], flex_ch: int) -> int:
    """flex 列名形如 flex0..flex4，根据通道号定位列。"""
    name = f"flex{flex_ch}"
    if name not in header:
        raise KeyError(f"CSV header 缺少 '{name}'；header={header}")
    return header.index(name)


def load_session(csv_path: Path) -> tuple[list[str], list[list[str]]]:
    """读取一个 raw.csv，返回 (header, rows)。"""
    with csv_path.open("r", encoding="utf-8", newline="") as fp:
        reader = csv.reader(fp)
        rows = list(reader)
    if not rows:
        return [], []
    header = rows[0]
    body = rows[1:]
    return header, body


# ---------------- 切窗 ----------------

def slice_windows(
    rows: list[list[str]],
    flex_col: int,
    label_col: int,
    win_size: int,
    stride: int,
) -> list[Window]:
    """对单个会话的行列表做滑窗切分。

    - 跳过 label<0（unlabeled）的行：以 label 切分成"段"，段长不足 1 窗的丢弃
    - 窗口内 label 必须全部一致才保留
    """
    if win_size <= 0 or stride <= 0:
        return []
    if not rows:
        return []

    # 先把脏行（列数不够）过滤掉
    cleaned: list[tuple[float, int]] = []  # (flex_value, label)
    for r in rows:
        if max(flex_col, label_col) >= len(r):
            continue
        try:
            flex_val = float(r[flex_col])
            lab = int(r[label_col])
        except ValueError:
            continue
        cleaned.append((flex_val, lab))

    # 按 label 段切（label 变化即新段）；只在同段内开窗
    out: list[Window] = []
    seg_start = 0
    cur_label = cleaned[0][1] if cleaned else 0
    for i in range(1, len(cleaned) + 1):
        # 段结束条件：到末尾，或 label 变化
        end_of_seg = (i == len(cleaned)) or (cleaned[i][1] != cur_label)
        if not end_of_seg:
            continue

        seg = cleaned[seg_start:i]
        # 段内 label 一致；只在 label>=0 时切窗
        if cur_label >= 0:
            for w_start in range(0, max(0, len(seg) - win_size + 1), stride):
                window = seg[w_start : w_start + win_size]
                if len(window) != win_size:
                    break
                samples = [v for (v, _) in window]
                out.append(Window(label=cur_label, samples=samples))

        # 进入下一段
        if i < len(cleaned):
            seg_start = i
            cur_label = cleaned[i][1]

    return out


# ---------------- 写 EI CSV ----------------

def write_ei_csv(
    windows: list[Window],
    out_dir: Path,
    frame_period_ms: int,
) -> None:
    """每个窗口写成一个 EI 兼容 CSV：
        timestamp,flex
        0,0.12
        50,0.13
        ...
    文件名格式：<label_name>.<seq>.csv
    """
    out_dir.mkdir(parents=True, exist_ok=True)
    seq_per_label: dict[int, int] = defaultdict(int)
    for w in windows:
        seq = seq_per_label[w.label]
        seq_per_label[w.label] += 1
        label_name = LABEL_NAMES.get(w.label, f"label{w.label}")
        path = out_dir / f"{label_name}.{seq:04d}.csv"
        with path.open("w", encoding="utf-8", newline="") as fp:
            writer = csv.writer(fp)
            writer.writerow(["timestamp", "flex"])
            for i, v in enumerate(w.samples):
                writer.writerow([i * frame_period_ms, f"{v:.6f}"])


# ---------------- 拆分 train/test ----------------

def split_train_test(
    windows: list[Window],
    test_ratio: float,
    seed: int,
) -> tuple[list[Window], list[Window]]:
    """按 label 分组打乱后拆分，保证每类的 train/test 比例一致。"""
    rng = random.Random(seed)
    by_label: dict[int, list[Window]] = defaultdict(list)
    for w in windows:
        by_label[w.label].append(w)

    train: list[Window] = []
    test: list[Window] = []
    for lab, ws in by_label.items():
        rng.shuffle(ws)
        n_test = max(1, int(round(len(ws) * test_ratio))) if len(ws) > 1 else 0
        test.extend(ws[:n_test])
        train.extend(ws[n_test:])
    return train, test


# ---------------- numpy 汇总 ----------------

def write_numpy(
    windows: list[Window],
    out_dir: Path,
    name: str,
    channel_count: int = 1,
) -> None:
    """写 X_<name>.npy / y_<name>.npy。"""
    if _np is None:
        print("[WARN] numpy 未安装，跳过 numpy 写出（pip install numpy 即可启用）",
              file=sys.stderr)
        return
    if not windows:
        print(f"[WARN] {name} 集合为空，跳过 numpy 写出", file=sys.stderr)
        return
    out_dir.mkdir(parents=True, exist_ok=True)
    n = len(windows)
    win_size = len(windows[0].samples)
    X = _np.zeros((n, win_size, channel_count), dtype=_np.float32)
    y = _np.zeros((n,), dtype=_np.int32)
    for i, w in enumerate(windows):
        # 单通道直接铺；后续多通道需调整 slice_windows 的输出
        X[i, :, 0] = w.samples
        y[i] = w.label
    _np.save(out_dir / f"X_{name}.npy", X)
    _np.save(out_dir / f"y_{name}.npy", y)
    print(f"[ok] numpy: X_{name}.shape={X.shape}  y_{name}.shape={y.shape}")


# ---------------- 主流程 ----------------

def collect_sessions(in_root: Path) -> list[Path]:
    """枚举 in_root 下所有单手 raw.csv（递归一层即可：session_*/raw.csv）。

    注意：会排除 `session_*_bimanual/` 后缀的双手目录——这些由 --bimanual
    分支走 collect_bimanual_sessions 处理。不过滤会导致双手 csv
    被单手 schema 误读（报 'flex1' 缺失）。
    """
    return sorted(
        p for p in in_root.glob("session_*/raw.csv")
        if not p.parent.name.endswith("_bimanual")
    )


def _filter_sessions_by_ids(
    sessions: list[Path], in_root: Path, allowed_ids: list[str]
) -> list[Path]:
    """按 GUI 传入的 session_id 白名单过滤 raw.csv 路径。

    session_id 即为 session_*/raw.csv 中的上级目录名（如 session_20260529_184311_bimanual）。
    允许传入不存在的 id（只警告不报错）。
    """
    if not allowed_ids:
        return sessions
    allowed = set(allowed_ids)
    kept: list[Path] = []
    found: set[str] = set()
    for p in sessions:
        sid = p.parent.name
        if sid in allowed:
            kept.append(p)
            found.add(sid)
    missing = allowed - found
    if missing:
        print(
            f"[warn] --sessions 指定的以下 id 未在 {in_root} 下找到："
            + ", ".join(sorted(missing)),
            file=sys.stderr,
        )
    return kept


# ---------------- bimanual 切窗 + numpy ----------------

def collect_bimanual_sessions(in_root: Path) -> list[Path]:
    """只枚举 session_*_bimanual/raw.csv（由 LingxiCapture bimanual writer 落盘）。"""
    return sorted(in_root.glob("session_*_bimanual/raw.csv"))


def slice_bimanual_windows(
    header: list[str],
    rows: list[list[str]],
    win_size: int,
    stride: int,
    max_slave_age_ms: int,
) -> tuple[list[BimanualWindow], int]:
    """对 bimanual raw.csv 切窗（26 通道）。

    过滤策略：
      - 列数不齐 / 数值 parse 失败：丢
      - slave_age_ms < 0 或 > max_slave_age_ms：整行丢（slave stale）
      - label<0 (unlabeled)：按段切，跳过
      - 段内 label 必须一致

    返回 (windows, dropped_stale_rows)。
    """
    if win_size <= 0 or stride <= 0 or not rows:
        return [], 0

    # 定位关键列
    try:
        feature_cols = [header.index(name) for name in BIMANUAL_FEATURE_COLS]
        label_col = header.index("label")
        age_col = header.index("slave_age_ms")
    except ValueError as e:
        raise KeyError(f"bimanual raw.csv header 不完整: {e}") from e

    # 清洗：列数齐 + label/age 解析成功 + age 过滤
    cleaned: list[tuple[list[float], int]] = []
    dropped_stale = 0
    max_idx = max(*feature_cols, label_col, age_col)
    for r in rows:
        if max_idx >= len(r):
            continue
        try:
            vec = [float(r[c]) for c in feature_cols]
            lab = int(r[label_col])
            age = int(r[age_col])
        except ValueError:
            continue
        # stale 过滤：age=-1（从未收到 slave）或 age 超阈值 → 丢
        if age < 0 or age > max_slave_age_ms:
            dropped_stale += 1
            continue
        cleaned.append((vec, lab))

    if not cleaned:
        return [], dropped_stale

    # 按 label 段切
    out: list[BimanualWindow] = []
    seg_start = 0
    cur_label = cleaned[0][1]
    for i in range(1, len(cleaned) + 1):
        end_of_seg = (i == len(cleaned)) or (cleaned[i][1] != cur_label)
        if not end_of_seg:
            continue
        seg = cleaned[seg_start:i]
        if cur_label >= 0:
            for w_start in range(0, max(0, len(seg) - win_size + 1), stride):
                window = seg[w_start : w_start + win_size]
                if len(window) != win_size:
                    break
                samples = [vec for (vec, _) in window]
                out.append(BimanualWindow(label=cur_label, samples=samples))
        if i < len(cleaned):
            seg_start = i
            cur_label = cleaned[i][1]
    return out, dropped_stale


def write_numpy_bimanual(
    windows: list[BimanualWindow],
    out_dir: Path,
    name: str,
) -> None:
    """写 bimanual X.shape=(N, win_size, 26)，y.shape=(N,)。"""
    if _np is None:
        print("[WARN] numpy 未安装，跳过 bimanual numpy 写出（pip install numpy 即可启用）",
              file=sys.stderr)
        return
    if not windows:
        print(f"[WARN] bimanual {name} 集合为空，跳过 numpy 写出", file=sys.stderr)
        return
    out_dir.mkdir(parents=True, exist_ok=True)
    n = len(windows)
    win_size = len(windows[0].samples)
    X = _np.zeros((n, win_size, BIMANUAL_CHANNEL_COUNT), dtype=_np.float32)
    y = _np.zeros((n,), dtype=_np.int32)
    for i, w in enumerate(windows):
        for t, vec in enumerate(w.samples):
            X[i, t, :] = vec
        y[i] = w.label
    _np.save(out_dir / f"X_bimanual_{name}.npy", X)
    _np.save(out_dir / f"y_bimanual_{name}.npy", y)
    print(f"[ok] bimanual numpy: X_{name}.shape={X.shape}  y_{name}.shape={y.shape}")


def run_bimanual(args, in_root: Path, out_root: Path) -> int:
    """bimanual 子流程：切窗 + 仅 numpy 输出（EI 多通道双手暂不直出）。"""
    sessions = collect_bimanual_sessions(in_root)
    if args.sessions:
        sessions = _filter_sessions_by_ids(sessions, in_root, args.sessions)
    if not sessions:
        print(f"[ERROR] 未在 {in_root} 下找到 session_*_bimanual/raw.csv", file=sys.stderr)
        return 2
    print(f"[info] [bimanual] 发现 {len(sessions)} 个会话")

    all_windows: list[BimanualWindow] = []
    label_counter: Counter[int] = Counter()
    total_stale = 0
    for sess in sessions:
        header, body = load_session(sess)
        if not header:
            print(f"[warn] {sess} 为空，跳过", file=sys.stderr)
            continue
        try:
            wins, stale = slice_bimanual_windows(
                header, body,
                win_size=args.window, stride=args.stride,
                max_slave_age_ms=args.max_slave_age_ms,
            )
        except KeyError as e:
            print(f"[warn] {sess} header 不合规: {e}", file=sys.stderr)
            continue
        for w in wins:
            label_counter[w.label] += 1
        total_stale += stale
        print(f"  {sess.relative_to(in_root)}: rows={len(body)}  "
              f"stale_dropped={stale}  windows={len(wins)}")
        all_windows.extend(wins)

    if not all_windows:
        print("[ERROR] bimanual 切窗后样本为零；可能 slave 长期 stale 或未打 label",
              file=sys.stderr)
        return 3

    print(f"\n[info] bimanual 切窗完成；总共丢弃 stale 行 {total_stale}")
    print("[info] 按 label 分布：")
    for lab, cnt in sorted(label_counter.items()):
        name = LABEL_NAMES.get(lab, f"label{lab}")
        print(f"  label={lab} ({name}): {cnt} 窗")

    train, test = split_train_test_bimanual(all_windows, args.test_ratio, args.seed)
    print(f"\n[info] bimanual train={len(train)}  test={len(test)}")

    if not args.no_numpy:
        write_numpy_bimanual(train, out_root, "train")
        write_numpy_bimanual(test, out_root, "test")

    print("\n[done] bimanual 数据集构建完毕")
    return 0


def split_train_test_bimanual(
    windows: list[BimanualWindow],
    test_ratio: float,
    seed: int,
) -> tuple[list[BimanualWindow], list[BimanualWindow]]:
    """按 label 分组打乱后拆分（与单手 split_train_test 对齐）。"""
    rng = random.Random(seed)
    by_label: dict[int, list[BimanualWindow]] = defaultdict(list)
    for w in windows:
        by_label[w.label].append(w)
    train: list[BimanualWindow] = []
    test: list[BimanualWindow] = []
    for lab, ws in by_label.items():
        rng.shuffle(ws)
        n_test = max(1, int(round(len(ws) * test_ratio))) if len(ws) > 1 else 0
        test.extend(ws[:n_test])
        train.extend(ws[n_test:])
    return train, test


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(
        description="LingxiGlove 数据集构建器：切窗 + EI CSV + numpy 汇总",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    parser.add_argument("--in", dest="in_root", default=str(DEFAULT_IN),
                        help="capture 会话根目录（含 session_*/raw.csv）")
    parser.add_argument("--out", default=str(DEFAULT_OUT),
                        help="输出根目录（含 ei_csv/ + npy）")
    parser.add_argument("--window", type=int, default=20,
                        help="窗口长度（帧）")
    parser.add_argument("--stride", type=int, default=10,
                        help="步长（帧）")
    parser.add_argument("--flex-channel", type=int, default=1,
                        help="使用的 flex 通道号（默认 1=食指）")
    parser.add_argument("--frame-period-ms", type=int, default=DEFAULT_FRAME_PERIOD_MS,
                        help="采样周期(ms)，写入 EI CSV 的 timestamp 列")
    parser.add_argument("--test-ratio", type=float, default=0.2,
                        help="测试集比例")
    parser.add_argument("--seed", type=int, default=42,
                        help="划分随机种子")
    parser.add_argument("--no-numpy", action="store_true",
                        help="禁用 numpy 汇总写出")
    parser.add_argument("--bimanual", action="store_true",
                        help="切到双手联合模式：消费 session_*_bimanual/raw.csv，"
                             "26 通道（13 master + 13 slave），仅输出 numpy")
    parser.add_argument("--max-slave-age-ms", type=int, default=200,
                        help="bimanual 模式下，slave_age_ms 大于此值的行被剔除（stale 过滤）")
    parser.add_argument("--sessions", nargs="+", default=None, metavar="SESSION_ID",
                        help="只处理指定的 session_id（session 目录名）列表；不传则处理全部。"
                             "示例：--sessions session_20260529_184311_bimanual session_20260529_162500_bimanual")
    args = parser.parse_args(argv)

    in_root = Path(args.in_root).expanduser().resolve()
    out_root = Path(args.out).expanduser().resolve()

    # bimanual 分支：完全独立流程
    if args.bimanual:
        return run_bimanual(args, in_root, out_root)

    sessions = collect_sessions(in_root)
    if args.sessions:
        sessions = _filter_sessions_by_ids(sessions, in_root, args.sessions)
    if not sessions:
        # 友好提示：检查是否有 bimanual 目录但用户忘了 --bimanual
        bimanual_found = collect_bimanual_sessions(in_root)
        if bimanual_found:
            print(
                f"[ERROR] 未找到单手 session_*/raw.csv，但发现 {len(bimanual_found)} 个双手 bimanual 会话。\n"
                f"        如需处理双手数据，请加 --bimanual 参数。",
                file=sys.stderr,
            )
        else:
            print(f"[ERROR] 未在 {in_root} 下找到 session_*/raw.csv", file=sys.stderr)
        return 2

    print(f"[info] 发现 {len(sessions)} 个会话")
    all_windows: list[Window] = []
    label_counter: Counter[int] = Counter()
    for sess in sessions:
        header, body = load_session(sess)
        if not header:
            print(f"[warn] {sess} 为空，跳过", file=sys.stderr)
            continue
        try:
            label_col = header.index("label")
            flex_col = _flex_column_index(header, args.flex_channel)
        except (ValueError, KeyError) as e:
            print(f"[warn] {sess} header 不合规: {e}", file=sys.stderr)
            continue

        wins = slice_windows(
            body, flex_col=flex_col, label_col=label_col,
            win_size=args.window, stride=args.stride,
        )
        for w in wins:
            label_counter[w.label] += 1
        print(f"  {sess.relative_to(in_root)}: rows={len(body)}  windows={len(wins)}")
        all_windows.extend(wins)

    if not all_windows:
        print("[ERROR] 切窗后样本为零；请检查端侧是否真的打了 label", file=sys.stderr)
        return 3

    print("\n[info] 切窗完成，按 label 分布：")
    for lab, cnt in sorted(label_counter.items()):
        name = LABEL_NAMES.get(lab, f"label{lab}")
        print(f"  label={lab} ({name}): {cnt} 窗")

    train, test = split_train_test(all_windows, args.test_ratio, args.seed)
    print(f"\n[info] train={len(train)}  test={len(test)}  test_ratio={args.test_ratio}")

    # 写 EI CSV
    ei_root = out_root / "ei_csv"
    write_ei_csv(train, ei_root / "train", args.frame_period_ms)
    write_ei_csv(test, ei_root / "test", args.frame_period_ms)
    print(f"[ok] EI CSV -> {ei_root}/{{train,test}}")

    # 写 numpy
    if not args.no_numpy:
        if _np is None:
            print("[WARN] numpy 不可用；如需 npy 汇总请 pip install numpy", file=sys.stderr)
        else:
            write_numpy(train, out_root, "train", channel_count=1)
            write_numpy(test, out_root, "test", channel_count=1)

    print("\n[done] 数据集构建完毕")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
