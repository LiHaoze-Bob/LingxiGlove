#ifndef LLM_CLIENT_H
#define LLM_CLIENT_H

#include <Arduino.h>

// 初始化LLM模块（获取access_token等）
bool initLLM();

// 调用大模型进行对话
// prompt: 用户输入的提示词
// 返回: 大模型的回复文本，失败返回空字符串
String chatLLM(const char* prompt);

// 获取百度TTS的access_token（TTS播放模块也会用到）
String getBaiduAccessToken();

#endif // LLM_CLIENT_H
