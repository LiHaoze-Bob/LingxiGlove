// ============================================================
// gesture_arbitrator.h
// 手势仲裁层 —— 统一决策单手/双手识别结果
// ============================================================
// 设计目标（方案 C：统一决策层）：
//   单手和双手识别器各自输出"候选 + 置信度"，
//   由仲裁层根据置信度、优先级、时序做最终决策。
//   该层模型无关：规则识别器或 CNN 均可接入。
// ============================================================

#ifndef GESTURE_ARBITRATOR_H
#define GESTURE_ARBITRATOR_H

#include <stdint.h>

// ------------------- 手势来源 -------------------
enum GestureSource {
    GESTURE_SOURCE_NONE        = 0,
    GESTURE_SOURCE_SINGLE_HAND = 1,
    GESTURE_SOURCE_BIMANUAL    = 2,
};

// ------------------- 仲裁候选 -------------------
struct GestureCandidate {
    GestureSource source;       // 来源：单手 / 双手 / 无
    const char*   text;         // 手势文本（空字符串或 NULL 表示无候选）
    float         confidence;   // 置信度 [0.0, 1.0]
};

// ------------------- 仲裁结果 -------------------
struct ArbitratedGesture {
    bool          should_announce;  // 本帧是否应播报
    GestureSource source;           // 最终选定的来源
    const char*   text;             // 最终文本（should_announce=false 时为 ""）
    float         confidence;       // 最终置信度
};

// ------------------- 仲裁器 -------------------
class GestureArbitrator {
public:
    GestureArbitrator();

    /**
     * @brief 初始化/重置仲裁器状态
     */
    void init();

    /**
     * @brief 每帧调用：提交两个识别器的候选，返回仲裁决策
     *
     * @param single   单手识别器本帧输出（source=SINGLE_HAND，无结果时 text="" 或 confidence=0）
     * @param bimanual 双手识别器本帧输出（source=BIMANUAL，无结果时 text="" 或 confidence=0）
     * @param now_ms   当前 millis()
     * @return 仲裁结果；should_announce=true 表示本帧应触发播报
     *
     * 决策规则：
     *   1. 双手候选存在时，获得优先级加成（BIMANUAL_PRIORITY_BOOST）
     *   2. 胜出候选需持续 CONFIRM_WINDOW_MS 才确认（防止瞬态误触）
     *   3. 播报后进入 COOLDOWN_MS 冷却期，期间不输出新结果
     *   4. 双手候选存在时，抑制单手播报（防止"帮助"被"不"抢先）
     */
    ArbitratedGesture tick(const GestureCandidate& single,
                           const GestureCandidate& bimanual,
                           unsigned long now_ms);

    /**
     * @brief 获取冷却剩余毫秒数（用于外部判断）
     */
    unsigned long cooldownRemaining(unsigned long now_ms) const;

private:
    // ---- 状态 ----
    const char*   m_pending_text_;      // 当前候选手势文本
    GestureSource m_pending_source_;    // 当前候选手势来源
    float         m_pending_confidence_;
    unsigned long m_pending_start_ms_;  // 候选开始时刻

    const char*   m_last_announced_text_;  // 上一次播报的文本
    unsigned long m_last_announce_ms_;     // 上一次播报时刻
    bool          m_cooldown_reset_done_;  // 冷却结束后是否已重置 pending

    // ---- 内部方法 ----
    bool isCandidateValid(const GestureCandidate& c) const;
    void resetPending();
};

#endif // GESTURE_ARBITRATOR_H
