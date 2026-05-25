// ============================================================
// gesture_arbitrator.cpp
// 手势仲裁层实现 —— 统一决策单手/双手识别结果
// ============================================================

#include "gesture_arbitrator.h"
#include "config.h"
#include <cstring>

// ---- 安全辅助：比较两个 C 字符串是否相等（容忍 NULL） ----
static bool TextEquals(const char* a, const char* b) {
    if (a == nullptr && b == nullptr) return true;
    if (a == nullptr || b == nullptr) return false;
    return std::strcmp(a, b) == 0;
}

static bool TextEmpty(const char* t) {
    return t == nullptr || t[0] == '\0';
}

// ============================================================
// 构造 / 初始化
// ============================================================

GestureArbitrator::GestureArbitrator()
    : m_pending_text_(nullptr)
    , m_pending_source_(GESTURE_SOURCE_NONE)
    , m_pending_confidence_(0.0f)
    , m_pending_start_ms_(0)
    , m_last_announced_text_(nullptr)
    , m_last_announce_ms_(0)
    , m_cooldown_reset_done_(false) {
}

void GestureArbitrator::init() {
    m_pending_text_        = nullptr;
    m_pending_source_      = GESTURE_SOURCE_NONE;
    m_pending_confidence_  = 0.0f;
    m_pending_start_ms_    = 0;
    m_last_announced_text_ = nullptr;
    m_last_announce_ms_    = 0;
    m_cooldown_reset_done_ = false;
}

// ============================================================
// 核心仲裁逻辑
// ============================================================

ArbitratedGesture GestureArbitrator::tick(
    const GestureCandidate& single,
    const GestureCandidate& bimanual,
    unsigned long now_ms)
{
    ArbitratedGesture result = { false, GESTURE_SOURCE_NONE, "", 0.0f };

    // ---- Step 1: 选择胜出候选 ----
    bool single_valid   = isCandidateValid(single);
    bool bimanual_valid = isCandidateValid(bimanual);

    const GestureCandidate* winner = nullptr;

    if (bimanual_valid) {
        winner = &bimanual;
    } else if (single_valid) {
        winner = &single;
    }

    // ---- Step 2: 冷却检查 + 冷却后重置 ----
    // 冷却期间：不确认播报，但仍跟踪 pending（用于调试/日志）。
    // 冷却结束后的首帧：重置 pending，让候选从本帧开始重新计时。
    if (m_last_announce_ms_ > 0) {
        unsigned long elapsed = now_ms - m_last_announce_ms_;
        if (elapsed < ARBITRATOR_COOLDOWN_MS) {
            // 仍在冷却中 → 跟踪 pending 但不播报
            if (winner != nullptr) {
                m_pending_text_       = winner->text;
                m_pending_source_     = winner->source;
                m_pending_confidence_ = winner->confidence;
            }
            m_cooldown_reset_done_ = false;
            return result;
        }
        // 冷却已结束且尚未重置 → 清除 pending，本帧作为新起点
        if (!m_cooldown_reset_done_) {
            resetPending();
            m_cooldown_reset_done_ = true;
        }
    }

    // ---- Step 3: 更新 pending 状态 ----
    if (winner != nullptr) {
        if (TextEquals(winner->text, m_pending_text_)) {
            // 同一手势持续 → 保持 pending 起始时间
            m_pending_confidence_ = winner->confidence;
            m_pending_source_     = winner->source;
        } else {
            // 新手势 → 重置 pending
            m_pending_text_        = winner->text;
            m_pending_source_      = winner->source;
            m_pending_confidence_  = winner->confidence;
            m_pending_start_ms_    = now_ms;
        }
    } else {
        // 无有效候选 → 清除 pending
        resetPending();
    }

    // ---- Step 4: 确认窗口检查 ----
    if (m_pending_text_ != nullptr &&
        (now_ms - m_pending_start_ms_) >= ARBITRATOR_CONFIRM_MS) {

        // ---- Step 5: 去重（不重复播报同一手势） ----
        if (!TextEquals(m_pending_text_, m_last_announced_text_)) {
            result.should_announce = true;
            result.source          = m_pending_source_;
            result.text            = m_pending_text_;
            result.confidence      = m_pending_confidence_;

            // 更新播报记录
            m_last_announced_text_ = m_pending_text_;
            m_last_announce_ms_    = now_ms;

            // 播报后重置 pending，防止连续触发
            resetPending();
        }
    }

    return result;
}

// ============================================================
// 辅助方法
// ============================================================

unsigned long GestureArbitrator::cooldownRemaining(unsigned long now_ms) const {
    if (m_last_announce_ms_ == 0) return 0;
    unsigned long elapsed = now_ms - m_last_announce_ms_;
    if (elapsed >= ARBITRATOR_COOLDOWN_MS) return 0;
    return ARBITRATOR_COOLDOWN_MS - elapsed;
}

bool GestureArbitrator::isCandidateValid(const GestureCandidate& c) const {
    return c.source != GESTURE_SOURCE_NONE
        && !TextEmpty(c.text)
        && c.confidence >= 0.0f;
}

void GestureArbitrator::resetPending() {
    m_pending_text_       = nullptr;
    m_pending_source_     = GESTURE_SOURCE_NONE;
    m_pending_confidence_ = 0.0f;
    m_pending_start_ms_   = 0;
}
