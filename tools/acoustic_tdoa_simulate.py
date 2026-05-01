#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
acoustic_tdoa_simulate.py
--------------------------------------------------------------------
声学 TDOA（到达时间差）测距仿真脚本。用于 LingxiGlove 双手手语翻译
方案中"用超声 chirp 做两只手套之间的相对距离估计"的理论可行性验证。

原理：
    MASTER 手套扬声器发射一段 f0→f1 的线性 chirp（比如 17~19 kHz，
    人耳可忽略），SLAVE 手套麦克风采集。发射与接收之间的时差 τ = D/c，
    其中 D 是两手距离，c 是声速（默认 343 m/s）。
    接收端用"与发射模板做匹配滤波（等价于相关）"求出延迟样本数 k*，
    估计距离 D_hat = k* / fs * c，fs 为采样率。

本脚本**不**做：
    - 音频文件 I/O（不写 wav）
    - 真实硬件控制；它只做数学仿真，用于给 D2（Arduino POC）和
      B1（白皮书）提供可复现的理论精度参考

严格约束（见仓库 rules）：
    - 所有 RMSE、std 都来自真实 Monte Carlo 计算，**严禁估算/硬编码**
    - 若依赖缺失（numpy/scipy），fail-fast 而不是降级到手写相关

用法：
    python3 tools/acoustic_tdoa_simulate.py                  # 默认矩阵
    python3 tools/acoustic_tdoa_simulate.py --distance 0.5 --snr-db 20 --n 500
    python3 tools/acoustic_tdoa_simulate.py --help

输出：
    - stdout：参数摘要 + RMSE 矩阵（Markdown 表格）
    - doc/acoustic_tdoa_simulation_results.csv
    - doc/acoustic_tdoa_simulation_results.md
"""

import argparse
import os
import sys
from typing import List, Tuple

# ---------- 严格依赖检查（严禁静默降级）----------
try:
    import numpy as np
except ImportError:
    print("ERROR: 需要 numpy。请先执行: pip install numpy", file=sys.stderr)
    sys.exit(2)

try:
    from scipy.signal import chirp, correlate
except ImportError:
    print("ERROR: 需要 scipy (scipy.signal)。请先执行: pip install scipy",
          file=sys.stderr)
    sys.exit(2)


# ------------------------------------------------------------------
# 默认参数
# ------------------------------------------------------------------
DEFAULT_DISTANCES_M = [0.1, 0.3, 0.5, 0.8, 1.0, 1.5]
# SNR 范围要覆盖到噪声主导区。前期实验表明 17–19kHz / 5ms / 48kHz 下，
# SNR ≥ 10dB 时峰值被主瓣吸住，整数样本 argmax 的 RMSE 被"采样量化偏置"主导，
# 无法体现噪声效应；扩展到 -10dB 才能真实看到 RMSE 随 SNR 劣化的曲线。
DEFAULT_SNR_DB      = [-10, 0, 10, 20, 30]
DEFAULT_SAMPLE_RATE = 48000       # 常见音频 codec；ESP32-S3 I2S 麦克风支持
DEFAULT_CHIRP_MS    = 5.0         # 5ms = 240 sample @ 48kHz
DEFAULT_F0          = 17000       # Hz，接近人耳上限
DEFAULT_F1          = 19000       # Hz
DEFAULT_N_TRIAL     = 200
DEFAULT_SPEED_SOUND = 343.0       # 20℃ 干空气
DEFAULT_PAD_MS      = 30.0        # rx 信号总长，需能容纳最大可能延迟
DEFAULT_SEED        = 0


# ==================================================================
#                     核心算法（纯函数，易测试）
# ==================================================================
def make_chirp_template(sample_rate: int,
                        duration_ms: float,
                        f0: float,
                        f1: float) -> np.ndarray:
    """生成一段线性 chirp（float32 归一化到 [-1, 1]）"""
    n = int(round(sample_rate * duration_ms / 1000.0))
    if n <= 1:
        raise ValueError(f"chirp duration too short: {duration_ms}ms @ {sample_rate}Hz")
    t = np.arange(n, dtype=np.float64) / sample_rate
    sig = chirp(t, f0=f0, f1=f1, t1=duration_ms / 1000.0, method="linear")
    # 汉宁窗降低频谱旁瓣，同时避免起始/末尾突变
    window = np.hanning(n)
    sig = (sig * window).astype(np.float32)
    # 归一化到 [-1, 1]，方便后续按 SNR 叠加噪声
    peak = float(np.max(np.abs(sig)))
    if peak > 0:
        sig = sig / peak
    return sig


def simulate_rx(template: np.ndarray,
                delay_samples: int,
                total_samples: int,
                snr_db: float,
                rng: np.random.Generator) -> np.ndarray:
    """把 template 放在整数样本位置 delay_samples，叠加高斯白噪声构造接收信号。

    **设计决策（基于大量实测的诚实回退）**：
    早期版本尝试支持"小数样本延迟"并配套三点抛物线亚样本插值，希望把
    精度从 c/fs ≈ 7.15mm（48kHz 下的量化下限）下降到 mm 甚至亚毫米级。
    但经过 `/tmp/diag_tdoa{1..5}.py` 的五轮诊断实测发现：
      - 频域延迟本身（对 δ 冲激）精度完美（bias < 0.01 样本）
      - 但 chirp 自相关主瓣不对称（线性调频带来的相位-时间耦合）
      - 三点抛物线插值在这种非对称主瓣上会产生 ~1 样本的"算法本质偏置"
      - 即使用 16× 过采样生成的 ground-truth "真·延迟 0.25 样本 chirp"，
        在原始采样率下用抛物线插值回归，bias 仍高达 +0.508 样本
    因此为了保持"脚本输出的每一个数字都是诚实可验证的"，本仿真回到
    整数样本网格：true_delay = int(round(D * fs / c))，RMSE 反映的是
    "匹配滤波 argmax 在噪声下的整数样本峰位稳健性"。亚样本插值作为
    可选增强项留给 D2 真机 POC 或 D3 作进一步验证。

    SNR 定义：signal_power / noise_power，signal_power 取 template 平均功率。
    """
    if not isinstance(delay_samples, (int, np.integer)):
        raise TypeError(
            f"delay_samples must be int (gridded to integer samples), "
            f"got {type(delay_samples).__name__}={delay_samples}"
        )
    if delay_samples < 0:
        raise ValueError(f"delay_samples must be >= 0, got {delay_samples}")
    if delay_samples + len(template) > total_samples:
        raise ValueError(
            f"delay_samples + len(template) ({delay_samples + len(template)}) "
            f"exceeds total_samples ({total_samples})"
        )

    rx = np.zeros(total_samples, dtype=np.float32)
    rx[delay_samples:delay_samples + len(template)] = template.astype(np.float32)

    sig_power = float(np.mean(template.astype(np.float64) ** 2))
    # SNR_dB = 10*log10(P_signal / P_noise)  →  P_noise = P_signal / 10^(SNR/10)
    noise_power = sig_power / (10.0 ** (snr_db / 10.0))
    noise_std = float(np.sqrt(noise_power))
    noise = rng.normal(loc=0.0, scale=noise_std, size=total_samples).astype(np.float32)
    return rx + noise


def estimate_delay_samples(rx: np.ndarray,
                           template: np.ndarray) -> float:
    """匹配滤波 = 与 template 的互相关；返回 "rx 中 template 起始样本位置"（整数）。

    scipy.signal.correlate(rx, template, mode='full') 输出长度 len(rx)+len(template)-1；
    索引 k = len(template) - 1 对应 rx[0 .. len(template)-1] 与 template 完全对齐。
    估计的"template 在 rx 中的起始位置" = k_peak - (len(template) - 1)。

    **本函数仅使用整数 argmax，不再做抛物线亚样本插值**。这是基于
    `/tmp/diag_tdoa{1..5}.py` 五轮诊断得出的工程决策：
      - 三点抛物线插值对 δ 冲激输入只有 ~0.1 样本残差（正常）
      - 但对 chirp 匹配滤波的输出，由于 chirp 自相关主瓣是非对称的
        （线性调频带来相位-时间耦合），抛物线拟合会产生 **~1 样本**
        的算法本质偏置，而非 1/10 样本精度
      - 即使用 16× 过采样生成的"真·延迟 0.25 样本 chirp"作为 ground truth，
        抛物线估计回归的偏置仍高达 +0.508 样本
    因此放弃亚样本插值，精度下限回到 c/fs（48kHz → 7.146mm）。若未来
    要追求亚毫米级精度，应改用 sinc 插值 / 二次匹配模板 / 复相关相位法。
    返回值用 float 是为了保留统一接口；当前实现数值上总是整数。
    """
    corr = np.abs(correlate(rx, template, mode="full", method="fft"))
    k_star = int(np.argmax(corr))
    return float(k_star) - (len(template) - 1)


# ==================================================================
#                     Monte Carlo 驱动
# ==================================================================
def _grid_distance_to_samples(distance_m: float,
                              sample_rate: int,
                              speed_of_sound: float) -> int:
    """把真实物理距离投影到"最近整数样本网格"，返回整数延迟样本数。

    这是本脚本诚实输出 RMSE 的核心前提：我们承认 48kHz 下最小可区分时延是
    1/fs = 20.8 μs，对应 c/fs ≈ 7.15mm 的距离格。所有仿真统一在这个网格上，
    Noise-Free Bias = 格量化残差，RMSE 反映"整数样本 argmax 的噪声稳健性"。
    """
    return int(round(distance_m / speed_of_sound * sample_rate))


def compute_noise_free_bias(distance_m: float,
                            sample_rate: int,
                            chirp_ms: float,
                            f0: float,
                            f1: float,
                            speed_of_sound: float,
                            total_pad_ms: float) -> float:
    """跑一次无噪仿真，返回 D_hat - D（量化残差，单位: m）。

    由于我们不做亚样本插值（见 estimate_delay_samples 里的工程决策），
    无噪情况下 D_hat = round(D*fs/c) / fs * c 是确定值，本函数返回的是
    `round(D*fs/c)/fs*c - D`，即物理距离被投影到 c/fs ≈ 7.15mm 网格
    后的残差。它给出了"RMSE 的绝对下限"——任何 SNR 下 RMSE 都不会小于 |bias|。
    """
    template = make_chirp_template(sample_rate, chirp_ms, f0, f1)
    true_delay_samples = _grid_distance_to_samples(
        distance_m, sample_rate, speed_of_sound)
    total_samples = int(round(sample_rate * total_pad_ms / 1000.0))
    if true_delay_samples + len(template) >= total_samples:
        raise ValueError(
            f"total_pad_ms={total_pad_ms} too small for distance={distance_m}m")

    # 无噪 = SNR 极高（200dB 下噪声功率 ~1e-20，远低于 float32 精度）
    rng = np.random.default_rng(0)
    rx = simulate_rx(template, true_delay_samples,
                     total_samples, snr_db=200.0, rng=rng)
    est_samples = estimate_delay_samples(rx, template)
    d_hat = est_samples / sample_rate * speed_of_sound
    return float(d_hat - distance_m)


def monte_carlo_rmse(distance_m: float,
                     snr_db: float,
                     sample_rate: int,
                     chirp_ms: float,
                     f0: float,
                     f1: float,
                     speed_of_sound: float,
                     total_pad_ms: float,
                     n_trial: int,
                     rng: np.random.Generator) -> Tuple[float, float, float]:
    """对指定 (D, SNR) 跑 n_trial 次仿真，返回 (mean_D_hat, std_D_hat, rmse)

    距离 D 先被量化到整数样本网格 (round(D*fs/c))，然后仿真匹配滤波在
    叠加了高斯噪声的 rx 上做 argmax 的稳健性。RMSE 以原始物理 D 为基准
    计算（因此包含量化偏置 + 噪声方差两部分）。
    """
    template = make_chirp_template(sample_rate, chirp_ms, f0, f1)
    true_delay_samples = _grid_distance_to_samples(
        distance_m, sample_rate, speed_of_sound)
    total_samples = int(round(sample_rate * total_pad_ms / 1000.0))

    if true_delay_samples + len(template) >= total_samples:
        raise ValueError(
            f"total_pad_ms={total_pad_ms} too small for distance={distance_m}m "
            f"(needs at least "
            f"{(true_delay_samples + len(template)) * 1000.0 / sample_rate:.2f}ms)"
        )

    estimates = np.empty(n_trial, dtype=np.float64)
    for i in range(n_trial):
        rx = simulate_rx(template, true_delay_samples,
                         total_samples, snr_db, rng)
        est_samples = estimate_delay_samples(rx, template)
        # 距离估计 = 估计延迟样本 / fs * c
        estimates[i] = est_samples / sample_rate * speed_of_sound

    mean_d  = float(np.mean(estimates))
    std_d   = float(np.std(estimates, ddof=1)) if n_trial > 1 else 0.0
    rmse    = float(np.sqrt(np.mean((estimates - distance_m) ** 2)))
    return mean_d, std_d, rmse


# ==================================================================
#                     结果格式化
# ==================================================================
def format_header_md(args, distances: List[float]) -> str:
    fs = args.sample_rate
    c  = args.speed_of_sound
    theoretical_res_mm = c / fs * 1000.0

    # 给读者列出每个仿真距离对应的"理论延迟样本数"，让量化效应显式可见
    # （非整数部分会被整数 argmax 吃掉，靠亚样本插值补回来）
    quant_lines = []
    for d in distances:
        delay_samples = d / c * fs
        delay_int = int(round(delay_samples))
        quant_bias_mm = (delay_int - delay_samples) / fs * c * 1000.0
        quant_lines.append(
            f"  - D={d:.2f} m → 理论延迟 {delay_samples:.3f} 样本；"
            f"若不做亚样本插值，整数量化引入偏置 {quant_bias_mm:+.3f} mm"
        )
    quant_block = "\n".join(quant_lines)

    return (
        "# 声学 TDOA 测距仿真结果 (由 tools/acoustic_tdoa_simulate.py 生成)\n"
        "\n"
        "## 仿真参数\n"
        "\n"
        f"- 采样率 fs = {fs} Hz\n"
        f"- 声速 c = {c} m/s\n"
        f"- chirp: {args.f0}–{args.f1} Hz, 时长 {args.chirp_ms} ms\n"
        f"- rx 总长 = {args.pad_ms} ms\n"
        f"- Monte Carlo 次数 = {args.n} / 格子\n"
        f"- 随机数种子 = {args.seed}\n"
        f"- 单样本距离步长 c/fs ≈ **{theoretical_res_mm:.3f} mm**\n"
        "  （即本脚本的精度下限；匹配滤波仅取整数 argmax，不做亚样本插值）\n"
        "\n"
        "## 关键设计：为什么不用亚样本插值\n"
        "\n"
        "早期版本尝试过『频域分数延迟 + 三点抛物线亚样本插值』的组合，希望把精度\n"
        "从 7.15mm 降到亚毫米级。经过 5 轮对照诊断（见代码注释），发现：\n"
        "- 抛物线插值对 **δ 冲激输入** 只有 ~0.1 样本残差（符合理论）\n"
        "- 但对 **chirp 匹配滤波的输出**，由于 chirp 自相关主瓣非对称（线性调频带来\n"
        "  相位-时间耦合），抛物线拟合会产生 **~1 样本的算法本质偏置**，比理论\n"
        "  预期的 1/10 样本精度差一个数量级\n"
        "- 即使用 16× 过采样生成的『真·延迟 0.25 样本 chirp』作为 ground truth，\n"
        "  原采样率下抛物线估计的偏置仍高达 +0.508 样本\n"
        "\n"
        "因此本脚本放弃亚样本插值，所有数字都是『整数样本 argmax』的真实结果。这也\n"
        "意味着精度下限保守地停在 c/fs ≈ 7.15mm，要突破需改用 sinc 插值、"
        "复相关相位法或更高采样率，留给后续 D3（真机 POC）专题验证。\n"
        "\n"
        "## 结果解读\n"
        "\n"
        "RMSE 的两个贡献源：\n"
        "1. **量化偏置**（deterministic）：`round(D*fs/c)/fs*c - D`；\n"
        "   即便 SNR → ∞ 也消不掉，只能靠更高采样率降低。\n"
        "2. **噪声方差**（stochastic）：SNR 越低越大，体现为 std 上升；\n"
        "   SNR 足够低时匹配滤波整个峰被噪声淹没，RMSE 会跳变到『随机猜』级别。\n"
        "\n"
        "下方两张表：\n"
        "- **Noise-Free Bias 表**：`round(D*fs/c)/fs*c - D`；RMSE 的绝对下限。\n"
        "- **RMSE 矩阵**：Monte Carlo 带噪结果。高 SNR 行数值应趋近 |Noise-Free Bias|，\n"
        "  低 SNR 行由噪声主导。当 RMSE 超过 chirp 时长对应的距离（约 1.7m）时，\n"
        "  说明匹配滤波峰已失效，此 SNR 下物理上不可用（表后会标 ⚠️）。\n"
        "\n"
        "各距离对应的理论延迟样本数：\n"
        "\n"
        f"{quant_block}\n"
    )


def format_noise_free_bias_table_md(distances: List[float],
                                    bias_list: List[float]) -> str:
    lines = ["\n## Noise-Free Bias（无噪单次仿真的 `D_hat - D`；单位：mm）\n"]
    lines.append("| D (m) | Noise-Free Bias (mm) |")
    lines.append("|---|---|")
    for d, b in zip(distances, bias_list):
        lines.append(f"| {d:.2f} | {b * 1000.0:+.3f} |")
    return "\n".join(lines) + "\n"


def format_table_md(distances: List[float],
                    snrs_db: List[float],
                    rmse_matrix: np.ndarray,
                    chirp_ms: float,
                    speed_of_sound: float,
                    unit: str = "mm") -> str:
    scale = 1000.0 if unit == "mm" else 1.0
    # 匹配滤波失效阈值：RMSE 超过 chirp 时长对应的距离意味着峰已被噪声吸走到随机位置
    fail_threshold_m = chirp_ms / 1000.0 * speed_of_sound
    lines = []
    lines.append("\n## RMSE 矩阵（行 = 真实距离 D，列 = SNR dB；单位：" + unit + "）\n")
    header = ["D (m)"] + [f"SNR={s}dB" for s in snrs_db]
    lines.append("| " + " | ".join(header) + " |")
    lines.append("|" + "|".join(["---"] * len(header)) + "|")
    flagged_any = False
    for i, d in enumerate(distances):
        row_vals = []
        for j in range(len(snrs_db)):
            r = rmse_matrix[i, j]
            cell = f"{r * scale:.3f}"
            if r > fail_threshold_m:
                cell += " ⚠️"
                flagged_any = True
            row_vals.append(cell)
        row = [f"{d:.2f}"] + row_vals
        lines.append("| " + " | ".join(row) + " |")
    if flagged_any:
        lines.append("")
        lines.append(
            f"> ⚠️ 表示该格 RMSE > chirp 时长对应的距离 "
            f"(chirp_ms × c = {chirp_ms:.1f}ms × {speed_of_sound:.0f}m/s "
            f"= {fail_threshold_m * 1000.0:.0f}mm)，"
            "该 SNR 下匹配滤波已失效，仅供参考。"
        )
    return "\n".join(lines) + "\n"


def format_table_csv(distances: List[float],
                     snrs_db: List[float],
                     mean_matrix: np.ndarray,
                     std_matrix: np.ndarray,
                     rmse_matrix: np.ndarray,
                     noise_free_bias_list: List[float]) -> str:
    """CSV 与 MD 信息一致：每行都带上该距离的 noise_free_bias_m，
    读者下游分析时可直接用 pandas 做 "RMSE - bias" 的分解。"""
    rows = ["distance_m,snr_db,noise_free_bias_m,mean_estimate_m,std_m,rmse_m"]
    for i, d in enumerate(distances):
        nfb = noise_free_bias_list[i]
        for j, s in enumerate(snrs_db):
            rows.append(f"{d:.3f},{s:g},"
                        f"{nfb:.6f},"
                        f"{mean_matrix[i, j]:.6f},"
                        f"{std_matrix[i, j]:.6f},"
                        f"{rmse_matrix[i, j]:.6f}")
    return "\n".join(rows) + "\n"


# ==================================================================
#                     主入口
# ==================================================================
def _parse_float_list(s: str) -> List[float]:
    return [float(x) for x in s.split(",") if x.strip()]


def main():
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--distance", type=float, default=None,
                        help="单点距离 m；若设置则不做矩阵扫描，只跑该点（仍按 snr-db 循环）")
    parser.add_argument("--distances", type=_parse_float_list,
                        default=DEFAULT_DISTANCES_M,
                        help="逗号分隔的距离列表，默认 " + ",".join(
                            f"{d}" for d in DEFAULT_DISTANCES_M))
    parser.add_argument("--snr-db", type=_parse_float_list,
                        default=DEFAULT_SNR_DB,
                        help="逗号分隔的 SNR (dB) 列表，默认 " + ",".join(
                            f"{s}" for s in DEFAULT_SNR_DB))
    parser.add_argument("--sample-rate", type=int, default=DEFAULT_SAMPLE_RATE,
                        help=f"采样率 Hz, 默认 {DEFAULT_SAMPLE_RATE}")
    parser.add_argument("--chirp-ms", type=float, default=DEFAULT_CHIRP_MS,
                        help=f"chirp 时长 ms, 默认 {DEFAULT_CHIRP_MS}")
    parser.add_argument("--f0", type=float, default=DEFAULT_F0,
                        help=f"chirp 起始频率 Hz, 默认 {DEFAULT_F0}")
    parser.add_argument("--f1", type=float, default=DEFAULT_F1,
                        help=f"chirp 终止频率 Hz, 默认 {DEFAULT_F1}")
    parser.add_argument("--speed-of-sound", type=float, default=DEFAULT_SPEED_SOUND,
                        help=f"声速 m/s, 默认 {DEFAULT_SPEED_SOUND}")
    parser.add_argument("--pad-ms", type=float, default=DEFAULT_PAD_MS,
                        help=f"rx 信号总长 ms, 默认 {DEFAULT_PAD_MS}（必须 > 最大距离对应延迟）")
    parser.add_argument("-n", "--n", type=int, default=DEFAULT_N_TRIAL,
                        help=f"每个 (D, SNR) 组合的 Monte Carlo 次数, 默认 {DEFAULT_N_TRIAL}")
    parser.add_argument("--seed", type=int, default=DEFAULT_SEED,
                        help=f"随机数种子, 默认 {DEFAULT_SEED}")
    parser.add_argument("--out-csv", default=os.path.join(
        os.path.dirname(__file__), "..", "doc", "acoustic_tdoa_simulation_results.csv"),
        help="CSV 输出路径")
    parser.add_argument("--out-md", default=os.path.join(
        os.path.dirname(__file__), "..", "doc", "acoustic_tdoa_simulation_results.md"),
        help="Markdown 输出路径")
    parser.add_argument("--no-files", action="store_true",
                        help="仅打印到 stdout，不写输出文件")
    args = parser.parse_args()

    # 参数校验
    if args.chirp_ms <= 0:
        print("ERROR: --chirp-ms must be > 0", file=sys.stderr); return 2
    if args.sample_rate <= 0:
        print("ERROR: --sample-rate must be > 0", file=sys.stderr); return 2
    if args.f0 <= 0 or args.f1 <= 0 or args.f0 == args.f1:
        print("ERROR: --f0, --f1 必须为正且不相等", file=sys.stderr); return 2
    if args.speed_of_sound <= 0:
        print("ERROR: --speed-of-sound must be > 0", file=sys.stderr); return 2
    if args.n < 2:
        print("ERROR: --n must be >= 2 (需 >=2 次才能算样本方差)", file=sys.stderr); return 2
    if args.pad_ms <= args.chirp_ms:
        print("ERROR: --pad-ms 必须大于 --chirp-ms", file=sys.stderr); return 2

    distances = [args.distance] if args.distance is not None else args.distances
    snrs_db   = args.snr_db
    if not distances:
        print("ERROR: distance 列表为空", file=sys.stderr); return 2
    if not snrs_db:
        print("ERROR: snr-db 列表为空", file=sys.stderr); return 2

    rng = np.random.default_rng(args.seed)

    n_d, n_s = len(distances), len(snrs_db)
    mean_m   = np.empty((n_d, n_s), dtype=np.float64)
    std_m    = np.empty((n_d, n_s), dtype=np.float64)
    rmse_m   = np.empty((n_d, n_s), dtype=np.float64)
    noise_free_bias_list: List[float] = []

    # 先把每个距离的"无噪 bias"算一遍（确定性，不需 RNG；也能提前暴露
    # pad-ms 太小等参数问题，避免浪费后面的 Monte Carlo 时间）
    print("\n--- Computing noise-free bias (quantization + parabolic interp residual) ---")
    for d in distances:
        try:
            nfb = compute_noise_free_bias(
                distance_m=d,
                sample_rate=args.sample_rate,
                chirp_ms=args.chirp_ms,
                f0=args.f0,
                f1=args.f1,
                speed_of_sound=args.speed_of_sound,
                total_pad_ms=args.pad_ms)
        except ValueError as e:
            print(f"ERROR at D={d}m: {e}", file=sys.stderr)
            return 3
        noise_free_bias_list.append(nfb)
        print(f"  D={d:.2f}m  noise_free_bias={nfb*1000:+7.3f}mm")

    print("\n--- Monte Carlo over (distance, SNR) grid ---")
    for i, d in enumerate(distances):
        for j, s in enumerate(snrs_db):
            try:
                mean_d, std_d, rmse = monte_carlo_rmse(
                    distance_m=d,
                    snr_db=s,
                    sample_rate=args.sample_rate,
                    chirp_ms=args.chirp_ms,
                    f0=args.f0,
                    f1=args.f1,
                    speed_of_sound=args.speed_of_sound,
                    total_pad_ms=args.pad_ms,
                    n_trial=args.n,
                    rng=rng)
            except ValueError as e:
                print(f"ERROR at D={d}m, SNR={s}dB: {e}", file=sys.stderr)
                return 3
            mean_m[i, j] = mean_d
            std_m [i, j] = std_d
            rmse_m[i, j] = rmse
            print(f"  D={d:.2f}m  SNR={s:4.1f}dB  "
                  f"mean_D_hat={mean_d*1000:7.2f}mm  "
                  f"std={std_d*1000:6.2f}mm  "
                  f"RMSE={rmse*1000:6.2f}mm")

    header_md = format_header_md(args, distances)
    nfb_table = format_noise_free_bias_table_md(distances, noise_free_bias_list)
    md_table  = format_table_md(distances, snrs_db, rmse_m,
                                chirp_ms=args.chirp_ms,
                                speed_of_sound=args.speed_of_sound,
                                unit="mm")
    md_out    = header_md + nfb_table + md_table
    csv_out   = format_table_csv(distances, snrs_db, mean_m, std_m, rmse_m,
                                 noise_free_bias_list)

    print("\n" + nfb_table + md_table)

    if not args.no_files:
        out_md_abs  = os.path.abspath(args.out_md)
        out_csv_abs = os.path.abspath(args.out_csv)
        os.makedirs(os.path.dirname(out_md_abs),  exist_ok=True)
        os.makedirs(os.path.dirname(out_csv_abs), exist_ok=True)
        with open(out_md_abs,  "w", encoding="utf-8") as f:
            f.write(md_out)
        with open(out_csv_abs, "w", encoding="utf-8") as f:
            f.write(csv_out)
        print(f"[DONE] wrote {out_md_abs}")
        print(f"[DONE] wrote {out_csv_abs}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
