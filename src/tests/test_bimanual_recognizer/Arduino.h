// ============================================================
// Arduino.h — host-side stub for test_bimanual_recognizer
// ------------------------------------------------------------
// gesture_recognizer.h / sensor_manager.h 都 #include <Arduino.h>。
// 这里提供刚好够用的符号让真实 gesture_recognizer.cpp 在 PC 上
// 可被编译：
//   - millis()：由 test_bimanual_recognizer.cpp 实现，读 g_test_now_ms
//   - min(a,b)：RuleBasedRecognizer::recognize 用到（虽然本测试不
//     调用 RuleBasedRecognizer，但 cpp 同一个文件里要编译过）
//   - <stdint.h>/<cmath>：fabs / int16_t / uint16_t 等
// ============================================================

#ifndef TEST_ARDUINO_H_STUB
#define TEST_ARDUINO_H_STUB

#include <stdint.h>
#include <stddef.h>
#include <cmath>
#include <cstdio>

// millis() 在测试 cpp 中实现，读 g_test_now_ms
unsigned long millis(void);

// Arduino 全局 min（实际是宏，这里用模板等价替代）
template <typename T>
static inline T min(T a, T b) { return (a < b) ? a : b; }

#endif  // TEST_ARDUINO_H_STUB
