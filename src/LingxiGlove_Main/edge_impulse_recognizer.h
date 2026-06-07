// ============================================================
// edge_impulse_recognizer.h
// Edge Impulse 模型驱动的单手手势识别器（B 篇：Flex 静态 3 类）
// ------------------------------------------------------------
// 与 RuleBasedRecognizer 共享 GestureRecognizer 抽象，由工厂函数
// createGestureRecognizer() 在编译期根据 RECOGNIZER_BACKEND 宏选择实例。
// 仅在 RECOGNIZER_BACKEND == RECOGNIZER_BACKEND_EDGE_IMPULSE 时
// 进行实质编译（.cpp 内整体 #if 包裹），其余情况下编译为空对象。
// ============================================================
//
// 设计要点：
//   - 输入：单通道 flexNorm[FLEX_INDEX]（左手食指）
//   - 窗口：EI 模型导出的 EI_CLASSIFIER_RAW_SAMPLES_PER_FRAME 帧（默认 20）
//   - 步长：内部维护环形缓冲；每帧推一个值，窗口满后每经过 STRIDE 帧
//           触发一次推理，避免 20Hz 主循环里每帧都做推理浪费 CPU
//   - 阈值：argmax 概率 > EI_CONFIDENCE_THRESHOLD 才视为有效命中
//   - label 字符串 → GestureType 的映射写死在 .cpp 里，与训练集严格对齐
// ============================================================

#ifndef EDGE_IMPULSE_RECOGNIZER_H
#define EDGE_IMPULSE_RECOGNIZER_H

#include "config.h"
#include "gesture_recognizer.h"

// 仅在该后端启用时定义类；否则避免用户误用工厂之外的入口
#if RECOGNIZER_BACKEND == RECOGNIZER_BACKEND_EDGE_IMPULSE

// 推理触发的滑窗步长（单位：帧；20Hz 下 5 帧 = 250ms）
#ifndef EI_INFERENCE_STRIDE_FRAMES
#define EI_INFERENCE_STRIDE_FRAMES   5
#endif

// argmax 概率阈值；低于此值视为 NONE
#ifndef EI_CONFIDENCE_THRESHOLD
#define EI_CONFIDENCE_THRESHOLD      0.70f
#endif

// 输出去重稳定时间（毫秒）；连续 N 次同一 label 才正式确认
// 与单手 GESTURE_STABLE_MS 保持同一量级（500ms ≈ 10 帧）
#ifndef EI_STABLE_MS
#define EI_STABLE_MS                 500
#endif

class EdgeImpulseRecognizer : public GestureRecognizer {
public:
    EdgeImpulseRecognizer();
    ~EdgeImpulseRecognizer() override;

    bool init() override;
    GestureResult recognize(const SensorData& data) override;
    const char* getName() const override { return "EdgeImpulse(Flex)"; }

private:
    // 环形缓冲：长度 = EI_CLASSIFIER_RAW_SAMPLES_PER_FRAME * 通道数
    // 通过指针延迟分配，避免在该后端关闭时占用 BSS
    float*   m_buffer;
    size_t   m_buffer_len;
    size_t   m_write_ix;       // 下一个写入位置（环形）
    size_t   m_filled;         // 已填充帧数（不超过 m_buffer_len）
    uint16_t m_frames_since_last_infer;

    // 防抖
    GestureType   m_last_detected;
    unsigned long m_stable_start_ms;
    bool          m_in_stable_state;

    // 把环形缓冲按时间顺序展开到 dst（长度 = m_buffer_len）
    void flatten_window(float* dst) const;
};

#endif  // RECOGNIZER_BACKEND == RECOGNIZER_BACKEND_EDGE_IMPULSE

#endif  // EDGE_IMPULSE_RECOGNIZER_H
