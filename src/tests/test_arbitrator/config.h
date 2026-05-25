// Stub config.h for test_arbitrator
// 仅提供仲裁器所需的宏定义，屏蔽真实 config.h 中的 Arduino 依赖。
#ifndef CONFIG_H
#define CONFIG_H

#include <cstdio>

#define DEBUG_PRINT(...)   std::printf(__VA_ARGS__)
#define DEBUG_PRINTLN(...) std::printf(__VA_ARGS__); std::printf("\n")
#define DEBUG_LOG(...)     std::printf(__VA_ARGS__); std::printf("\n")

// 仲裁器配置（与 LingxiGlove_Main/config.h 保持一致）
#define ARBITRATOR_CONFIRM_MS   200
#define ARBITRATOR_COOLDOWN_MS  2000

#endif // CONFIG_H
