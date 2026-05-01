// ============================================================
// offline_voice_pcm.h
// 离线语音 PCM 表（Flash 常量数据，用于 TTS 失败时的兜底播报）
// ------------------------------------------------------------
// 设计约定：
//   1. 每条 PCM 记录必须是 16-bit signed mono，以便直接通过 PlayPcmInt16
//      写入 tts_player 的 I2S（采样率任意，每条自带 sample_rate 字段）。
//   2. **默认本表为空** (kOfflinePcmCount = 0)，避免任何伪造 / 预估数据
//      伪装成"可用离线兜底"。真实数据由 tools/gen_offline_voice_pcm.py
//      生成后覆盖本文件。
//   3. 表空时 PlayOfflineVoice 恒返回 false，让调用方上报"播报失败"——
//      这不是 bug，是"没数据就不假装能播"的安全默认行为。
//
// 使用方：local_tts_fallback.cpp
// ============================================================

#ifndef OFFLINE_VOICE_PCM_H
#define OFFLINE_VOICE_PCM_H

#include <stdint.h>
#include <stddef.h>

/**
 * @brief 单条离线语音的元数据 + 数据指针
 *
 * - label   ：匹配键（一般使用手势识别器的 GestureResult::text，比如 "你好"）
 * - data    ：16-bit signed mono PCM 数组（指向 PROGMEM/Flash 常量）
 * - sample_count ：样本数（字节数 = sample_count * 2）
 * - sample_rate  ：采样率 (Hz)，必须在 [8000, 48000]
 */
struct OfflinePcmEntry {
    const char*    label;
    const int16_t* data;
    size_t         sample_count;
    uint32_t       sample_rate;
};

// 空表——未注入数据前保持为空，严禁填充任何占位/伪造数据
extern const OfflinePcmEntry kOfflinePcmTable[];
extern const size_t          kOfflinePcmCount;

#endif  // OFFLINE_VOICE_PCM_H
