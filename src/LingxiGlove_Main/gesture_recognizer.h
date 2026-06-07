// ============================================================
// gesture_recognizer.h
// 手势识别引擎 - 抽象层设计，支持多种实现切换
// ============================================================
// MVP阶段: RuleBasedRecognizer（基于MPU6050姿态角的硬编码规则）
// 全功能阶段: EdgeImpulseRecognizer（替换为Edge Impulse C++模型）
// ============================================================

#ifndef GESTURE_RECOGNIZER_H
#define GESTURE_RECOGNIZER_H

#include <Arduino.h>
#include "sensor_manager.h"

// ------------------- 手势类型枚举 -------------------
enum GestureType {
    GESTURE_NONE = 0,
    GESTURE_HELLO,      // 你好
    GESTURE_THANKS,     // 谢谢
    GESTURE_GOODBYE,    // 再见
    GESTURE_YES,        // 是
    GESTURE_NO,         // 不
    // ---- B 篇：Flex 静态 3 类（由 EdgeImpulseRecognizer 输出）----
    // 仅在 RECOGNIZER_BACKEND == RECOGNIZER_BACKEND_EDGE_IMPULSE 时会被返回；
    // 保留枚举是为了与 RuleBasedRecognizer 共用同一抽象接口、同一个
    // s_gestureTexts 表，避免为 3 类静态手势再引入一套平行枚举。
    GESTURE_FLEX_STRAIGHT,  // 伸直
    GESTURE_FLEX_HALF,      // 半弯
    GESTURE_FLEX_FULL,      // 握拳
    GESTURE_COUNT
};

// ------------------- 手势识别结果 -------------------
struct GestureResult {
    GestureType type;       // 手势类型
    const char* text;       // 对应中文文本
    float confidence;       // 置信度 0.0~1.0
};

// ------------------- 抽象基类 -------------------
class GestureRecognizer {
public:
    virtual ~GestureRecognizer() {}

    // 初始化识别器（加载模型/配置参数）
    virtual bool init() = 0;

    // 根据传感器数据识别手势
    // 返回识别结果，未识别时 type = GESTURE_NONE
    virtual GestureResult recognize(const SensorData& data) = 0;

    // 获取识别器名称（用于日志显示）
    virtual const char* getName() const = 0;
};

// ------------------- MVP阶段：规则识别器 -------------------
// 基于 MPU6050 的 pitch/roll 姿态角做硬编码规则判定
class RuleBasedRecognizer : public GestureRecognizer {
public:
    bool init() override;
    GestureResult recognize(const SensorData& data) override;
    const char* getName() const override { return "RuleBased(MPU6050)"; }

private:
    // 判定阈值（度）
    static constexpr float ANGLE_THRESHOLD = 45.0f;
    static constexpr float NEUTRAL_ZONE    = 20.0f;

    // 防抖相关状态
    GestureType m_lastDetected;     // 上次检测到的手势
    unsigned long m_stableStartMs;  // 当前手势开始稳定的时间
    bool m_inStableState;           // 是否处于稳定状态
};

// ------------------- 全功能阶段：Edge Impulse 识别器 -------------------
// 将在模型训练完成并导出 Arduino 库后新增 EdgeImpulseRecognizer 类与对应实现。
// 届时需要同步确定：窗口长度、采样率、特征通道布局、label→GestureType 映射表，
// 再在此处声明类，并在 createGestureRecognizer() 中接入。当前阶段不预留任何空壳。

// ------------------- 工厂函数 -------------------
// 创建当前阶段适用的识别器实例。当前仅返回 RuleBasedRecognizer。
// 调用者负责 delete 返回的指针。
GestureRecognizer* createGestureRecognizer();

// ============================================================
// 双手协同识别器（ENABLE_ESPNOW_SYNC=1 && ESPNOW_ROLE=MASTER 时使用）
// ============================================================

/**
 * @brief 双手手势类型枚举
 *
 * 当前仅实现"加油"一个词，用于验证 ESP-NOW 双板通信链路。
 * 后续扩充词汇时在此枚举追加条目，并在 BimanualRuleRecognizer::recognize
 * 中补充对应规则。
 */
enum BimanualGestureType {
    BIMANUAL_GESTURE_NONE  = 0,
    BIMANUAL_GESTURE_JIAYOU,    // 加油：双手同时 pitch > BIMANUAL_PITCH_THRESHOLD_DEG
    BIMANUAL_GESTURE_YIQI,      // 一起：双手同时 pitch < BIMANUAL_PITCH_DOWN_THRESHOLD_DEG
    BIMANUAL_GESTURE_WOAINI,    // 我爱你：双手 roll 对称偏转（右手左倾+左手右倾）
    BIMANUAL_GESTURE_BANGZHU,   // 帮助：左手掌朝上托举 + 右手握拳置左掌（pitch/roll 居中）
    BIMANUAL_GESTURE_COUNT
};

/**
 * @brief 双手识别器的输入：Master 本帧 + Slave 最新帧（已由调用方换算为物理单位）
 *
 * 调用方（LingxiGlove_Main.ino）负责：
 *   1. 把本帧 SensorData 的 pitch 填入 master_pitch；
 *   2. 把最近收到的 Slave HandFrame 原始 ax/az 换算成 slave_pitch 并填入；
 *   3. 用 slave_frame_age_ms 告知 Slave 帧的"年龄"（millis() - 收帧时刻）；
 *      若 Slave 帧年龄超过 BIMANUAL_SLAVE_STALE_MS，identify 将直接返回 NONE。
 */
struct BimanualInput {
    float master_pitch;         // Master 手（右手）当前俯仰角，单位 °
    float slave_pitch;          // Slave  手（左手）最新俯仰角，单位 °
    float master_roll;          // Master 手（右手）当前横滚角，单位 °
    float slave_roll;           // Slave  手（左手）最新横滚角，单位 °
    unsigned long slave_frame_age_ms;  // Slave 帧距离现在的毫秒数
};

/**
 * @brief 双手识别结果
 */
struct BimanualGestureResult {
    BimanualGestureType type;   // 手势类型，NONE 表示未识别
    const char*         text;   // 对应中文文本（type=NONE 时为 ""）
    float               confidence; // 置信度 0.0~1.0
};

/**
 * @brief 双手规则识别器
 *
 * 无继承关系，与 GestureRecognizer（单手基类）完全独立，避免强行套接口。
 * 内部维护防抖计时器，对外仅暴露 init() + recognize()。
 */
class BimanualRuleRecognizer {
public:
    BimanualRuleRecognizer();

    /**
     * @brief 初始化识别器（重置防抖状态）
     */
    void init();

    /**
     * @brief 根据双手输入数据识别手势
     *
     * @param input  双手输入（master_pitch / slave_pitch / slave_frame_age_ms）
     * @return 识别结果；type=NONE 表示本帧无触发
     */
    BimanualGestureResult recognize(const BimanualInput& input);

private:
    BimanualGestureType  m_last_detected_;   // 上一帧判定的候选手势
    unsigned long        m_stable_start_ms_; // 候选手势稳定开始的时刻
    bool                 m_in_stable_state_; // 是否已进入稳定确认状态
};

#endif // GESTURE_RECOGNIZER_H
