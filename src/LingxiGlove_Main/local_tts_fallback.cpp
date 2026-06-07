// ============================================================
// local_tts_fallback.cpp
// 见 local_tts_fallback.h 头部说明
// ============================================================

#include "local_tts_fallback.h"
#include "offline_voice_pcm.h"
#include "tts_player.h"
#include "config.h"    // DEBUG_PRINTLN / DEBUG_PRINT 宏定义

#include <Arduino.h>   // PlayPcmInt16 已自带校验

#include <string.h>

size_t OfflineVoiceCount() {
    return kOfflinePcmCount;
}

bool PlayOfflineVoice(const char* label) {
    if (!label || label[0] == '\0') {
        DEBUG_PRINTLN("[离线TTS] label 为空");
        return false;
    }
    if (kOfflinePcmCount == 0) {
        // 空表是"合法但没数据"的状态，不是错误；不打印红色日志，避免每次调用都刷屏
        return false;
    }

    for (size_t i = 0; i < kOfflinePcmCount; i++) {
        const OfflinePcmEntry& e = kOfflinePcmTable[i];
        if (!e.label || !e.data) continue;  // 表项自身损坏时跳过
        if (strcmp(e.label, label) != 0) continue;

        DEBUG_LOG("[离线TTS] 匹配 label='%s' samples=%u rate=%u",
                  label, (uint32_t)e.sample_count, (uint32_t)e.sample_rate);

        if (!PlayPcmInt16(e.data, e.sample_count, e.sample_rate, label)) {
            DEBUG_PRINTLN("[离线TTS] I2S 播放失败");
            return false;
        }
        return true;
    }

    // 未命中：返回 false 让上层决定是否进一步处理
    return false;
}
