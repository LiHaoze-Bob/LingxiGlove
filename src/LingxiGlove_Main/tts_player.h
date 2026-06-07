#ifndef TTS_PLAYER_H
#define TTS_PLAYER_H

#include <Arduino.h>

// I2S 主时钟对应的默认采样率；与 initTTS() 里的 i2s_config.sample_rate 保持一致。
// 使用 24kHz 与 Qwen-TTS 输出格式一致，消除采样率来回切换导致的"滋滋"杂音。
#define TTS_I2S_DEFAULT_SAMPLE_RATE  24000u

// 初始化I2S音频输出
bool initTTS();

// 云端 TTS 合成并播放（支持 LittleFS 本地缓存加速）
// text:       要朗读的中文文本（建议不超过100字）
// cache_key:  缓存索引键（可选）。非空时用此键查找/写入缓存，
//             解决 LLM 改写不稳定（同一手势词每次改写结果不同）导致
//             缓存永远无法命中的问题。典型用法：传入原始手势词。
//             为 nullptr 时退化为以 text 本身作为缓存键。
// cache_only: true 时仅查本地缓存，命中则播放并返回 true，
//             未命中直接返回 false（不走云端合成）。
//             典型用法：在 LLM 改写之前先尝试缓存播放，命中则跳过 LLM 调用，
//             延迟从 3-5 秒降至 <100ms。
// 返回: true=播放成功, false=播放失败
bool speak(const char* text, const char* cache_key = nullptr, bool cache_only = false);

// 播放测试音（正弦波）
void playTestTone(int freq, int durationMs);

/**
 * @brief 清除 LittleFS 上的 TTS 缓存文件。
 *
 * 删除 /tts_cache/ 目录下所有 .wav 文件。
 * 下次 speak() 调用时会重新从云端合成并写入缓存。
 * TTS_CACHE_ENABLE=0 时本函数为空操作。
 */
void clearTtsCache();

/**
 * @brief 直接播放内存中的 int16 单声道 PCM 数据（离线兜底/声学 POC 等场景使用）。
 *
 * 供 local_tts_fallback 等模块复用 I2S 硬件通道，无需再次初始化驱动。
 *
 * 约束：
 *   - I2S 在 initTTS 中按 16-bit / Mono / LEFT 初始化，本函数仅支持同格式的 PCM；
 *   - sample_rate 支持偏离默认 TTS_I2S_DEFAULT_SAMPLE_RATE 的情况，函数内部会
 *     临时调用 i2s_set_sample_rates 切换，并在播放结束后恢复到默认采样率；
 *   - 参数必须经过严格校验（非空指针 + 合理采样率 + 合理长度），任何非法输入返回 false；
 *   - 本函数是**阻塞**的，时长约为 samples / sample_rate 秒。
 *
 * @param pcm          指向 int16 PCM 样本数组，必须非空
 * @param sample_count 样本数（样本数 × 2 = 字节数）。必须 > 0 且 ≤ 10*sample_rate（最多 10 秒）
 * @param sample_rate  采样率，Hz。必须在 [8000, 48000] 区间
 * @param label        可选的字幕标签（演示模式下作为 tts_audio 首帧的 text 字段，
 *                     便于 APP 显示原文）。nullptr 表示不带字幕。
 * @return true 成功，false 参数非法或 I2S 未初始化
 */
bool PlayPcmInt16(const int16_t* pcm, size_t sample_count, uint32_t sample_rate,
                  const char* label = nullptr);

#endif // TTS_PLAYER_H
