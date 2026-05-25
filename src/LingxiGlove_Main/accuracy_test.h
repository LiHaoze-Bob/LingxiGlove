// ============================================================
// accuracy_test.h
// 手势识别离线准确率测试模块
// ------------------------------------------------------------
// 用途：评测当前规则识别器（或未来 ML 模型）的真实命中率、置信度分布
// 和稳定时间，给优化提供量化基线。
//
// 工作流程：
//   1. 用户串口输入 "test <gesture_id> <count>"，进入测试会话
//   2. 主循环每帧调用 TickAccuracyTest()，传入识别结果与运动状态
//   3. 每次手势触发或超时后记录一行 CSV，提示用户回到 STILL 进入下一轮
//   4. 全部完成后打印汇总 + 写入 LittleFS `/acc_test/sNNN.csv`
//   5. 用户可用 "test export" 把所有历史日志 dump 到串口（便于拷贝分析）
//
// 重要特性：
//   - **不**触发 TTS / LLM，节省时间和云端费用
//   - **不**修改 g_lastAnnouncedGesture / g_lastAnnounceTime（不干扰识别器状态机）
//   - 用 MotionDetector 的 STILL 信号作为"用户已放松，准备下一轮"的判据
// ============================================================

#ifndef ACCURACY_TEST_H
#define ACCURACY_TEST_H

#include <stdint.h>
#include <stddef.h>

// 测试支持的手势 ID
// 单手手势直接映射 GestureType，双手手势用 100+ 区段避免冲突
enum AccuracyTestGestureId {
    ACC_TEST_GESTURE_NONE     = 0,
    // 单手（与 gesture_recognizer.h GestureType 编号一致，便于查表）
    ACC_TEST_GESTURE_HELLO    = 1,   // 你好
    ACC_TEST_GESTURE_THANKS   = 2,   // 谢谢
    ACC_TEST_GESTURE_GOODBYE  = 3,   // 再见
    ACC_TEST_GESTURE_YES      = 4,   // 是
    ACC_TEST_GESTURE_NO       = 5,   // 不
    // 双手（100 + BimanualGestureType）
    ACC_TEST_GESTURE_JIAYOU   = 101, // 加油
    ACC_TEST_GESTURE_YIQI     = 102, // 一起
    ACC_TEST_GESTURE_WOAINI   = 103, // 我爱你
    ACC_TEST_GESTURE_BANGZHU  = 104, // 帮助
};

/**
 * @brief 启动一次准确率测试会话
 *
 * 调用后状态机切到 TEST_WAITING_GESTURE，主循环开始接收 tick。
 *
 * @param gesture_id  目标手势 ID（见 AccuracyTestGestureId）
 * @param total       计划尝试次数（1~ACCURACY_TEST_MAX_ATTEMPTS）
 * @return true 启动成功；false 参数非法或 LittleFS 不可用
 */
bool StartAccuracyTest(uint16_t gesture_id, uint16_t total);

// 识别结果来源：用于区分单手 / 双手识别器，
// 模块根据当前目标 target_id 是否在 100+ 区段决定接受哪一类。
enum AccuracyTestSource {
    ACC_SOURCE_SINGLE_HAND = 0,
    ACC_SOURCE_BIMANUAL    = 1,
};

/**
 * @brief 主循环每帧调用，驱动测试状态机
 *
 * 单手和双手识别器各自调用一次此函数；不匹配当前测试目标的来源会被
 * 忽略其 detected 结果，但仍可推进"等待静止"阶段的状态机。
 *
 * @param now_ms          millis() 当前时刻
 * @param source          识别结果来源（单手 / 双手）
 * @param detected_label  本帧识别结果（NULL 或 "" 表示未识别）
 * @param confidence      本帧置信度（识别器输出，0~1）
 * @param pitch           当前 pitch（°），用于日志
 * @param roll            当前 roll（°）
 * @param motion_is_still MotionDetector 当前是否报告 STILL
 */
void TickAccuracyTest(unsigned long now_ms,
                      AccuracyTestSource source,
                      const char* detected_label,
                      float confidence,
                      float pitch, float roll,
                      bool motion_is_still);

/**
 * @brief 当前是否在测试会话中
 * 主循环用它判断要不要走正常 TTS/LLM 路径
 */
bool IsAccuracyTestActive();

/**
 * @brief 取消当前会话（不写入 CSV）
 */
void CancelAccuracyTest();

/**
 * @brief 把 LittleFS `/acc_test/` 下所有历史 CSV dump 到串口
 *
 * 调用方负责保证此时不在测试会话中（is_active=false）。
 */
void ExportAccuracyTestLogs();

/**
 * @brief 清空 LittleFS `/acc_test/` 下所有日志 + 重置会话计数器
 */
void ClearAccuracyTestLogs();

#endif  // ACCURACY_TEST_H
