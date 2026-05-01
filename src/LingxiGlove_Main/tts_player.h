#ifndef TTS_PLAYER_H
#define TTS_PLAYER_H

#include <Arduino.h>

// I2S 主时钟对应的默认采样率；与 initTTS() 里的 i2s_config.sample_rate 保持一致
#define TTS_I2S_DEFAULT_SAMPLE_RATE  16000u

// 初始化I2S音频输出
bool initTTS();

// 使用百度TTS将文本转为语音并播放
// text: 要朗读的中文文本（建议不超过100字）
// 返回: true=播放成功, false=播放失败
bool speak(const char* text);

// 播放测试音（正弦波）
void playTestTone(int freq, int durationMs);

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
 * @return true 成功，false 参数非法或 I2S 未初始化
 */
bool PlayPcmInt16(const int16_t* pcm, size_t sample_count, uint32_t sample_rate);

#endif // TTS_PLAYER_H
