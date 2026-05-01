#!/usr/bin/env python3
"""
gen_chirp_pcm.py
---------------------------------------------------
为 D2 的 Arduino POC (src/tests/test_acoustic_tdoa/test_acoustic_tdoa.ino)
生成 17–19 kHz 线性 chirp 的 int16 PCM 模板，写入 chirp_pcm.h。

强制约束（与 D1 脚本 tools/acoustic_tdoa_simulate.py 完全一致，方便 D3 真机
数据与 D1 仿真结果直接比对）：
  - 采样率  fs       = 48000 Hz
  - 时长    dur_ms   = 5.0 ms            → 240 个样本
  - 频率范围 f0→f1   = 17000 → 19000 Hz
  - 加 Hann 窗避免起止突变
  - 归一化到 int16 峰值 ~28000（留 ~15% headroom，防 MAX98357A DAC 削波）

输出：
  src/tests/test_acoustic_tdoa/chirp_pcm.h
    - 定义 kChirpSampleRate / kChirpLen
    - 定义 const int16_t kChirpPcm[kChirpLen] PROGMEM = {...}

使用：
  python3 tools/gen_chirp_pcm.py
  # 或自定义输出路径：
  python3 tools/gen_chirp_pcm.py --out src/tests/test_acoustic_tdoa/chirp_pcm.h
"""
import argparse
import os
import sys

try:
    import numpy as np
except ImportError:
    print("ERROR: 需要 numpy。请先执行: pip install numpy", file=sys.stderr)
    sys.exit(1)

try:
    from scipy.signal import chirp
except ImportError:
    print("ERROR: 需要 scipy。请先执行: pip install scipy", file=sys.stderr)
    sys.exit(1)


# ==================================================================
# 默认参数（与 tools/acoustic_tdoa_simulate.py 保持一致）
# ==================================================================
DEFAULT_SAMPLE_RATE = 48000
DEFAULT_CHIRP_MS    = 5.0
DEFAULT_F0          = 17000.0
DEFAULT_F1          = 19000.0
# 给 MAX98357A 留 headroom：int16 满刻度 32767，用 28000 约 -1.4 dB
DEFAULT_PEAK_I16    = 28000


def generate_chirp_int16(sample_rate: int,
                         duration_ms: float,
                         f0: float,
                         f1: float,
                         peak_i16: int) -> np.ndarray:
    n = int(round(sample_rate * duration_ms / 1000.0))
    if n <= 1:
        raise ValueError(
            f"chirp duration too short: {duration_ms}ms @ {sample_rate}Hz")
    t = np.arange(n, dtype=np.float64) / sample_rate
    sig = chirp(t, f0=f0, f1=f1, t1=duration_ms / 1000.0, method="linear")
    sig *= np.hanning(n)
    peak = float(np.max(np.abs(sig)))
    if peak > 0:
        sig = sig / peak * peak_i16
    return np.round(sig).astype(np.int16)


def format_header(samples: np.ndarray,
                  sample_rate: int,
                  duration_ms: float,
                  f0: float,
                  f1: float,
                  peak_i16: int) -> str:
    rows = []
    rows.append("// chirp_pcm.h — 由 tools/gen_chirp_pcm.py 自动生成，请勿手工编辑")
    rows.append("//")
    rows.append("// D2 声学 TDOA POC 所用的线性 chirp 模板（int16，单声道 PCM）。")
    rows.append(f"// 参数: fs={sample_rate} Hz, dur={duration_ms} ms ({len(samples)} samples),")
    rows.append(f"//       f0={f0:g} Hz → f1={f1:g} Hz, Hann window,")
    rows.append(f"//       normalized peak = {peak_i16} (leaves ~{(32767-peak_i16)/32768*100:.1f}% headroom)")
    rows.append("//")
    rows.append("// 必须与 tools/acoustic_tdoa_simulate.py 的参数完全一致，才能把")
    rows.append("// D3 真机实测数据直接对照 D1 的仿真 RMSE 表格。")
    rows.append("")
    rows.append("#ifndef CHIRP_PCM_H_")
    rows.append("#define CHIRP_PCM_H_")
    rows.append("")
    rows.append("#include <stdint.h>")
    rows.append("#include <pgmspace.h>  // PROGMEM (Arduino/ESP32)")
    rows.append("")
    # 说明：用 constexpr 保证 kChirpLen 可以作为 C++11 数组长度的整型常量表达式。
    # 非 constexpr 的 static const 在部分严格编译器 (如 -pedantic) 下用作数组长度
    # 会告警；POC 直接统一成 constexpr 最稳妥。
    rows.append("// 用 constexpr 保证 kChirpLen 可以作为 C++11 数组长度的整型常量表达式。")
    rows.append("// 非 constexpr 的 static const 在部分严格编译器 (如 -pedantic) 下用作数组长度")
    rows.append("// 会告警；POC 直接统一成 constexpr 最稳妥。")
    rows.append(f"constexpr uint32_t kChirpSampleRate = {sample_rate}u;")
    rows.append(f"constexpr uint32_t kChirpLen        = {len(samples)}u;")
    rows.append(f"constexpr float    kChirpF0Hz       = {f0:.1f}f;")
    rows.append(f"constexpr float    kChirpF1Hz       = {f1:.1f}f;")
    rows.append(f"constexpr float    kChirpDurMs      = {duration_ms:.3f}f;")
    rows.append("")
    rows.append("static const int16_t kChirpPcm[kChirpLen] PROGMEM = {")
    # 每行 12 个样本，对齐格式
    per_line = 12
    for i in range(0, len(samples), per_line):
        chunk = samples[i:i + per_line]
        line = "    " + ", ".join(f"{int(v):6d}" for v in chunk) + ","
        rows.append(line)
    # 去掉最后一个逗号以避免一些极端编译器的 trailing-comma 告警（C/C++ 实际都允许，
    # 但显式去掉让代码更干净）
    rows[-1] = rows[-1].rstrip(",")
    rows.append("};")
    rows.append("")
    rows.append("#endif  // CHIRP_PCM_H_")
    rows.append("")
    return "\n".join(rows)


def main():
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--sample-rate", type=int, default=DEFAULT_SAMPLE_RATE)
    parser.add_argument("--chirp-ms",    type=float, default=DEFAULT_CHIRP_MS)
    parser.add_argument("--f0",          type=float, default=DEFAULT_F0)
    parser.add_argument("--f1",          type=float, default=DEFAULT_F1)
    parser.add_argument("--peak-i16",    type=int,   default=DEFAULT_PEAK_I16,
                        help="int16 归一化峰值，默认 28000（-1.4dBFS，防 DAC 削波）")
    parser.add_argument("--out", default="src/tests/test_acoustic_tdoa/chirp_pcm.h",
                        help="输出头文件相对路径（相对于仓库根）")
    args = parser.parse_args()

    if args.sample_rate <= 0:
        print("ERROR: --sample-rate must be > 0", file=sys.stderr); return 2
    if args.chirp_ms <= 0:
        print("ERROR: --chirp-ms must be > 0", file=sys.stderr); return 2
    if not (0 < args.f0 < args.sample_rate / 2) or \
       not (0 < args.f1 < args.sample_rate / 2):
        print(f"ERROR: f0/f1 must be in (0, fs/2)=(0,{args.sample_rate/2})",
              file=sys.stderr); return 2
    if not (0 < args.peak_i16 <= 32767):
        print("ERROR: --peak-i16 must be in (0, 32767]", file=sys.stderr); return 2

    samples = generate_chirp_int16(args.sample_rate, args.chirp_ms,
                                   args.f0, args.f1, args.peak_i16)
    text = format_header(samples, args.sample_rate, args.chirp_ms,
                         args.f0, args.f1, args.peak_i16)

    out_path = os.path.abspath(args.out)
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    with open(out_path, "w", encoding="utf-8") as f:
        f.write(text)

    peak_actual = int(np.max(np.abs(samples)))
    print(f"[DONE] wrote {out_path}")
    print(f"  len = {len(samples)} samples ({args.chirp_ms}ms @ {args.sample_rate}Hz)")
    print(f"  freq range = {args.f0:g} → {args.f1:g} Hz")
    print(f"  peak (int16) = {peak_actual} (target {args.peak_i16})")
    print(f"  header size (bytes) ≈ {os.path.getsize(out_path):,}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
