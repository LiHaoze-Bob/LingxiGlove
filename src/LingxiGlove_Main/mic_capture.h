// ============================================================
// mic_capture.h
// INMP441 I2S 麦克风采集模块（PTT 期间专用）
//
// 职责边界：
//   - 模块只负责 I2S 配置 + DMA 缓冲读取 + PCM 块输出
//   - 不做 ASR、不做编码（PCM 16-bit LE 直送 WS）、不做 VAD
//   - 与 TTS 播放（MAX98357A on I2S0）解耦：本模块用 I2S1
//   - PTT 双击后由上层 Start()，握拳后由上层 Stop()
//
// 当前阶段：ENABLE_MIC_CAPTURE = 0，所有函数为安全 stub，编译可过、
//             不引入 driver/i2s 依赖。当物理麦克风到位后，仅需翻开开关。
//
// 引脚：
//   - I2S1 BCLK = D10  (MIC_I2S_BCLK)
//   - I2S1 LRCLK = D11 (MIC_I2S_LRCLK)
//   - I2S1 DIN   = D12 (MIC_I2S_DIN)
//   - VCC = 3.3V，L/R = GND（左声道，与 test_acoustic_tdoa 保持一致）
// ============================================================
#ifndef MIC_CAPTURE_H
#define MIC_CAPTURE_H

#include <stdint.h>
#include <stddef.h>

#include "config.h"

#if !defined(ENABLE_MIC_CAPTURE)
#define ENABLE_MIC_CAPTURE 0
#endif

#ifdef __cplusplus
extern "C" {
#endif

// 一次性安装 I2S1 + DMA 缓冲区。返回 false 表示初始化失败（如 ENABLE_MIC_CAPTURE=0
// 时也允许直接返回 true，主循环可继续，本模块进入空跳过模式）。
bool MicCapture_Init();

// 启动 I2S 接收：清空 DMA 缓冲并使 I2S 进入运行态。
// 上层在 PttDecision.start_recording 为真时调用。
bool MicCapture_Start();

// 停止 I2S 接收，但不销毁驱动。上层在 PttDecision.stop_recording 为真时调用。
bool MicCapture_Stop();

// 卸载 I2S 驱动（通常程序退出 / 模块禁用时使用）
void MicCapture_Deinit();

// 是否正在采集
bool MicCapture_IsRunning();

// 读取一块 PCM。返回写入的样本数（int16_t 个数，单声道），0 表示无数据。
// out_buffer 必须 >= MIC_CHUNK_SAMPLES。
size_t MicCapture_ReadChunk(int16_t* out_buffer, size_t max_samples);

// 给前端 MIC 卡片用的实时电平（0~1，由内部对最近一块做 RMS 估算）。
// 在 ENABLE_MIC_CAPTURE=0 或未启动时返回 0.0f。
float MicCapture_GetLevel();

#ifdef __cplusplus
}
#endif

#endif  // MIC_CAPTURE_H
