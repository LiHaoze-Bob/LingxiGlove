// ============================================================
// local_tts_fallback.h
// 在线 TTS 失败时的本地兜底播报
// ------------------------------------------------------------
// 职责：
//   根据 label 在 offline_voice_pcm.h 的表里找到对应条目，
//   通过 tts_player.h::PlayPcmInt16 播出。
//
// 非职责：
//   - 不做文本→语音合成（纯查表）
//   - 不伪造蜂鸣之类的"有声就行"兜底（空表时直接返回 false）
// ============================================================

#ifndef LOCAL_TTS_FALLBACK_H
#define LOCAL_TTS_FALLBACK_H

#include <stdbool.h>
#include <stddef.h>    // size_t 的规范头

/**
 * @brief 按 label 精确匹配查表并通过 I2S 播放。
 *
 * @param label 要播放的文本标签（与 GestureResult::text 一致，字符串精确相等）
 * @return true 已成功播放；false 表示 label 为空 / 表为空 / 未命中 / I2S 播放失败
 */
bool PlayOfflineVoice(const char* label);

/**
 * @brief 返回离线表中已注入的条目数。0 表示无可用兜底数据。
 */
size_t OfflineVoiceCount();

#endif  // LOCAL_TTS_FALLBACK_H
