// ============================================================
// accuracy_test.cpp
// 手势识别离线准确率测试模块
// ============================================================

#include "accuracy_test.h"

#include <Arduino.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <string.h>

#include "config.h"   // DEBUG_LOG/PRINTLN + ACCURACY_TEST_* 常量

// ============================================================
// 内部状态
// ============================================================
enum AccTestPhase {
    PHASE_IDLE = 0,
    PHASE_WAITING_GESTURE,
    PHASE_WAITING_REST,
    PHASE_DONE
};

static struct {
    AccTestPhase phase;
    uint16_t target_id;
    char     target_label[16];   // UTF-8，足够装下"我爱你"等
    uint16_t total;
    uint16_t current_attempt;    // 1-based
    uint16_t session_id;

    unsigned long slot_start_ms;
    unsigned long rest_still_since_ms;  // 进入 STILL 的最早时刻

    File     csv_file;

    // 汇总累计
    uint16_t hit_count;
    uint16_t miss_count;
    uint16_t mistake_count;
    float    confidence_sum;
    uint32_t detection_ms_sum;
    uint32_t detection_ms_max;
} g_state;

// ============================================================
// 辅助
// ============================================================
static const char* LabelOfId(uint16_t id) {
    switch (id) {
        case ACC_TEST_GESTURE_HELLO:   return "你好";
        case ACC_TEST_GESTURE_THANKS:  return "谢谢";
        case ACC_TEST_GESTURE_GOODBYE: return "再见";
        case ACC_TEST_GESTURE_YES:     return "是";
        case ACC_TEST_GESTURE_NO:      return "不";
        case ACC_TEST_GESTURE_JIAYOU:  return "加油";
        case ACC_TEST_GESTURE_YIQI:    return "一起";
        case ACC_TEST_GESTURE_WOAINI:  return "我爱你";
        case ACC_TEST_GESTURE_BANGZHU: return "帮助";
        default:                       return nullptr;
    }
}

static uint16_t AllocateNextSessionId() {
    Preferences prefs;
    if (!prefs.begin("acc_test", false)) {
        return 1;
    }
    uint16_t next_id = prefs.getUShort("next_id", 1);
    prefs.putUShort("next_id", (uint16_t)(next_id + 1));
    prefs.end();
    return next_id;
}

static void ResetSessionCounter() {
    Preferences prefs;
    if (prefs.begin("acc_test", false)) {
        prefs.putUShort("next_id", 1);
        prefs.end();
    }
}

static bool EnsureFs() {
    if (!LittleFS.begin(true)) {
        DEBUG_PRINTLN("[准确率测试] LittleFS 不可用（检查分区方案是否含 SPIFFS）");
        return false;
    }
    return true;
}

static void FinishSession() {
    uint16_t total = g_state.total;
    float recall = (total > 0) ? (float)g_state.hit_count / (float)total : 0.f;
    float avg_conf = (g_state.hit_count > 0)
                     ? g_state.confidence_sum / g_state.hit_count : 0.f;
    uint32_t avg_det = (g_state.hit_count > 0)
                       ? g_state.detection_ms_sum / g_state.hit_count : 0;

    if (g_state.csv_file) {
        char buf[200];
        g_state.csv_file.write((const uint8_t*)"\n", 1);
        int n = snprintf(buf, sizeof(buf),
                         "# summary: hits=%u, misses=%u, mistakes=%u\n",
                         g_state.hit_count, g_state.miss_count, g_state.mistake_count);
        if (n > 0) g_state.csv_file.write((const uint8_t*)buf, (size_t)n);
        n = snprintf(buf, sizeof(buf),
                     "# recall=%.3f, avg_conf=%.3f, avg_det_ms=%lu, max_det_ms=%lu\n",
                     (double)recall, (double)avg_conf,
                     (unsigned long)avg_det, (unsigned long)g_state.detection_ms_max);
        if (n > 0) g_state.csv_file.write((const uint8_t*)buf, (size_t)n);
        g_state.csv_file.close();
    }

    DEBUG_PRINTLN("\n============= 准确率测试汇总 =============");
    DEBUG_LOG("  会话 s%03u   目标: %s   总次数: %u",
              g_state.session_id, g_state.target_label, total);
    DEBUG_LOG("  命中 %u   漏报 %u   误识 %u",
              g_state.hit_count, g_state.miss_count, g_state.mistake_count);
    DEBUG_LOG("  召回率:       %.1f%%", (double)(recall * 100.f));
    DEBUG_LOG("  平均置信度:   %.2f", (double)avg_conf);
    DEBUG_LOG("  平均检出时长: %lu ms", (unsigned long)avg_det);
    DEBUG_LOG("  最差检出时长: %lu ms", (unsigned long)g_state.detection_ms_max);
    DEBUG_PRINTLN("==========================================");
    DEBUG_PRINTLN("[准确率测试] 日志已写入 LittleFS；用 'test export' 导出全部历史");
}

// ============================================================
// 公开接口
// ============================================================
bool StartAccuracyTest(uint16_t gesture_id, uint16_t total) {
    if (IsAccuracyTestActive()) {
        DEBUG_PRINTLN("[准确率测试] 当前已在测试中，请先 'test cancel' 或等待完成");
        return false;
    }
    const char* label = LabelOfId(gesture_id);
    if (!label) {
        DEBUG_LOG("[准确率测试] 未知手势 id=%u", gesture_id);
        return false;
    }
    if (total == 0 || total > ACCURACY_TEST_MAX_ATTEMPTS) {
        DEBUG_LOG("[准确率测试] 次数非法 (1~%d)", ACCURACY_TEST_MAX_ATTEMPTS);
        return false;
    }
    if (!EnsureFs()) return false;

    LittleFS.mkdir("/acc_test");

    g_state.session_id = AllocateNextSessionId();
    char path[40];
    snprintf(path, sizeof(path), "/acc_test/s%03u.csv", g_state.session_id);
    g_state.csv_file = LittleFS.open(path, "w");
    if (!g_state.csv_file) {
        DEBUG_LOG("[准确率测试] 无法打开日志文件 %s", path);
        return false;
    }
    {
        char header[160];
        int n = snprintf(header, sizeof(header),
                         "# session_id=%u, target=%s, total=%u, started_at_ms=%lu\n",
                         g_state.session_id, label, total,
                         (unsigned long)millis());
        if (n > 0) g_state.csv_file.write((const uint8_t*)header, (size_t)n);
        const char* hdr = "attempt_idx,truth,detected,confidence,detection_ms,pitch,roll\n";
        g_state.csv_file.write((const uint8_t*)hdr, strlen(hdr));
    }

    g_state.phase = PHASE_WAITING_GESTURE;
    g_state.target_id = gesture_id;
    strncpy(g_state.target_label, label, sizeof(g_state.target_label) - 1);
    g_state.target_label[sizeof(g_state.target_label) - 1] = '\0';
    g_state.total = total;
    g_state.current_attempt = 1;
    g_state.slot_start_ms = millis();
    g_state.rest_still_since_ms = 0;
    g_state.hit_count = 0;
    g_state.miss_count = 0;
    g_state.mistake_count = 0;
    g_state.confidence_sum = 0.f;
    g_state.detection_ms_sum = 0;
    g_state.detection_ms_max = 0;

    DEBUG_LOG("\n[准确率测试] 会话 s%03u 开始：目标=%s  计划次数=%u",
              g_state.session_id, label, total);
    DEBUG_LOG("[准确率测试] 单次超时 %d ms，做完后回到中性姿态自动进入下一轮",
              ACCURACY_TEST_SLOT_TIMEOUT_MS);
    DEBUG_LOG("[准确率测试] 进度 (1/%u)：现在打出 %s", total, label);
    return true;
}

bool IsAccuracyTestActive() {
    return g_state.phase == PHASE_WAITING_GESTURE ||
           g_state.phase == PHASE_WAITING_REST;
}

void CancelAccuracyTest() {
    if (g_state.phase == PHASE_IDLE) return;
    if (g_state.csv_file) {
        g_state.csv_file.println();
        g_state.csv_file.println("# canceled by user");
        g_state.csv_file.close();
    }
    DEBUG_LOG("[准确率测试] 已取消会话 s%03u（已完成 %u/%u 次）",
              g_state.session_id,
              (unsigned)(g_state.current_attempt - 1),
              g_state.total);
    g_state.phase = PHASE_IDLE;
}

void TickAccuracyTest(unsigned long now_ms,
                      AccuracyTestSource source,
                      const char* detected_label,
                      float confidence,
                      float pitch, float roll,
                      bool motion_is_still) {
    if (!IsAccuracyTestActive()) return;

    // 来源过滤：当前目标与识别来源不匹配时，detected 视为无效
    // （但仍允许此 tick 推进 timeout / rest 状态机）
    bool target_is_bimanual = (g_state.target_id >= 100);
    bool source_is_bimanual = (source == ACC_SOURCE_BIMANUAL);
    if (target_is_bimanual != source_is_bimanual) {
        detected_label = nullptr;
    }

    if (g_state.phase == PHASE_WAITING_GESTURE) {
        bool detected = (detected_label != nullptr && detected_label[0] != '\0');

        if (detected) {
            uint32_t det_ms = (uint32_t)(now_ms - g_state.slot_start_ms);
            bool is_hit = (strcmp(detected_label, g_state.target_label) == 0);

            if (g_state.csv_file) {
                char row[160];
                int n = snprintf(row, sizeof(row),
                                 "%u,%s,%s,%.3f,%lu,%.2f,%.2f\n",
                                 g_state.current_attempt,
                                 g_state.target_label,
                                 detected_label,
                                 (double)confidence,
                                 (unsigned long)det_ms,
                                 (double)pitch, (double)roll);
                if (n > 0) g_state.csv_file.write((const uint8_t*)row, (size_t)n);
                g_state.csv_file.flush();
            }

            if (is_hit) {
                g_state.hit_count++;
                g_state.confidence_sum += confidence;
                g_state.detection_ms_sum += det_ms;
                if (det_ms > g_state.detection_ms_max) {
                    g_state.detection_ms_max = det_ms;
                }
                DEBUG_LOG("[准确率测试] (%u/%u) ✓ %s  conf=%.2f  耗时=%lums",
                          g_state.current_attempt, g_state.total,
                          detected_label, (double)confidence,
                          (unsigned long)det_ms);
            } else {
                g_state.mistake_count++;
                DEBUG_LOG("[准确率测试] (%u/%u) ✗ 误识 %s（应=%s）  conf=%.2f",
                          g_state.current_attempt, g_state.total,
                          detected_label, g_state.target_label,
                          (double)confidence);
            }

            g_state.phase = PHASE_WAITING_REST;
            g_state.rest_still_since_ms = 0;
            return;
        }

        // 超时漏报
        if ((now_ms - g_state.slot_start_ms) > ACCURACY_TEST_SLOT_TIMEOUT_MS) {
            if (g_state.csv_file) {
                char row[160];
                int n = snprintf(row, sizeof(row),
                                 "%u,%s,,0.000,0,%.2f,%.2f\n",
                                 g_state.current_attempt,
                                 g_state.target_label,
                                 (double)pitch, (double)roll);
                if (n > 0) g_state.csv_file.write((const uint8_t*)row, (size_t)n);
                g_state.csv_file.flush();
            }
            g_state.miss_count++;
            DEBUG_LOG("[准确率测试] (%u/%u) ⚠ 超时漏报",
                      g_state.current_attempt, g_state.total);

            g_state.phase = PHASE_WAITING_REST;
            g_state.rest_still_since_ms = 0;
        }
        return;
    }

    if (g_state.phase == PHASE_WAITING_REST) {
        if (!motion_is_still) {
            g_state.rest_still_since_ms = 0;
            return;
        }
        if (g_state.rest_still_since_ms == 0) {
            g_state.rest_still_since_ms = now_ms;
            return;
        }
        if ((now_ms - g_state.rest_still_since_ms) <
            ACCURACY_TEST_REST_HOLD_MS) {
            return;
        }
        // 静止时间够了 → 推进
        g_state.current_attempt++;
        if (g_state.current_attempt > g_state.total) {
            FinishSession();
            g_state.phase = PHASE_DONE;
        } else {
            g_state.phase = PHASE_WAITING_GESTURE;
            g_state.slot_start_ms = now_ms;
            DEBUG_LOG("[准确率测试] 进度 (%u/%u)：现在打出 %s",
                      g_state.current_attempt, g_state.total,
                      g_state.target_label);
        }
    }
}

void ExportAccuracyTestLogs() {
    if (!EnsureFs()) return;
    File dir = LittleFS.open("/acc_test");
    if (!dir || !dir.isDirectory()) {
        DEBUG_PRINTLN("[准确率测试] 无历史日志");
        if (dir) dir.close();
        return;
    }
    int count = 0;
    File f = dir.openNextFile();
    while (f) {
        if (!f.isDirectory()) {
            DEBUG_LOG("\n========= %s (%u bytes) =========",
                      f.name(), (unsigned)f.size());
            char buf[256];
            while (f.available()) {
                size_t nread = f.readBytes(buf, sizeof(buf));
                if (nread == 0) break;
                Serial.write((const uint8_t*)buf, nread);
            }
            Serial.println();
            count++;
        }
        f.close();
        f = dir.openNextFile();
    }
    dir.close();
    DEBUG_LOG("[准确率测试] 共导出 %d 个会话", count);
}

void ClearAccuracyTestLogs() {
    if (!EnsureFs()) return;
    File dir = LittleFS.open("/acc_test");
    if (!dir || !dir.isDirectory()) {
        if (dir) dir.close();
        ResetSessionCounter();
        DEBUG_PRINTLN("[准确率测试] 目录不存在，仅复位计数器");
        return;
    }

    // 先收集路径再删除，避免迭代中删除引发的未定义行为
    char paths[64][40];
    int n = 0;
    File f = dir.openNextFile();
    while (f && n < 64) {
        if (!f.isDirectory()) {
            const char* name = f.name();
            if (name[0] == '/') {
                strncpy(paths[n], name, sizeof(paths[n]) - 1);
            } else {
                snprintf(paths[n], sizeof(paths[n]),
                         "/acc_test/%s", name);
            }
            paths[n][sizeof(paths[n]) - 1] = '\0';
            n++;
        }
        f.close();
        f = dir.openNextFile();
    }
    dir.close();

    int removed = 0;
    for (int i = 0; i < n; i++) {
        if (LittleFS.remove(paths[i])) removed++;
    }
    ResetSessionCounter();
    DEBUG_LOG("[准确率测试] 已清空 %d 个日志（扫描 %d 个），计数器已复位",
              removed, n);
}
