#ifndef LLM_CLIENT_H
#define LLM_CLIENT_H

#include <Arduino.h>

// 初始化LLM模块（获取access_token等）
bool initLLM();

// 调用大模型进行对话
// prompt: 用户输入的提示词
// 返回: 大模型的回复文本，失败返回空字符串
String chatLLM(const char* prompt);

/**
 * @brief 把一段"手势词/字母/短序列"改写成一句自然中文口语。
 *
 * 典型输入：
 *   - "你好"         → 期望输出 "你好呀"
 *   - "吃饭"         → 期望输出 "我想吃饭"
 *   - "我,吃饭"      → 期望输出 "我要吃饭"
 *   - "H,E,L,L,O"    → 期望输出 "你好" 或 "Hello"
 *
 * 实现上对 chatLLM 做一层薄封装：拼固定 prompt 模板 → 调模型 → 去首尾空白/
 * 引号 / 换行 → 做长度 sanity check（> LLM_REWRITE_MAX_BYTES 视为异常）。
 *
 * 失败（LLM 返回空 / 以 "[错误]" 开头 / 超长 / 未配置提供商）统一返回空 String。
 * 调用方应在拿到空串时回落到原始 gesture_sequence，保证播报链路永不断。
 *
 * @param gesture_sequence 非空 C 字符串；多个手势间的分隔符不限，交给 LLM 理解
 * @return 改写后的自然句；失败返回空 String
 */
String rewriteGestureToSentence(const char* gesture_sequence);

// 获取百度TTS的access_token（TTS播放模块也会用到）
String getBaiduAccessToken();

#endif // LLM_CLIENT_H
