// ============================================================
// calibration.cpp
// 见 calibration.h 头部的设计说明
// ============================================================

#include "calibration.h"
#include "sensor_manager.h"   // setImuBias / setFlexRuntimeRange 注入接口
#include "config.h"           // DEBUG_PRINTLN / DEBUG_PRINT 宏定义

#include <Arduino.h>
#include <Preferences.h>

// ------------------- NVS 常量 -------------------
static const char* kNvsNamespace = "lingxi_cal";
static const char* kKeyVersion   = "ver";
static const char* kKeyFlags     = "flags";
static const char* kKeyAxBias    = "ax_bias";
static const char* kKeyAyBias    = "ay_bias";
static const char* kKeyAzBias    = "az_bias";
static const char* kKeyGxBias    = "gx_bias";
static const char* kKeyGyBias    = "gy_bias";
static const char* kKeyGzBias    = "gz_bias";
#if ENABLE_FLEX_SENSORS
static const char* kKeyFlexMin[FLEX_CHANNEL_COUNT] = {
    "fmin0", "fmin1", "fmin2", "fmin3", "fmin4"
};
static const char* kKeyFlexMax[FLEX_CHANNEL_COUNT] = {
    "fmax0", "fmax1", "fmax2", "fmax3", "fmax4"
};
#endif

// ------------------- 基础辅助 -------------------
void ResetCalibrationStruct(CalibrationData* cal) {
    if (!cal) return;
    cal->accel_bias_x = 0.0f;
    cal->accel_bias_y = 0.0f;
    cal->accel_bias_z = 0.0f;
    cal->gyro_bias_x  = 0.0f;
    cal->gyro_bias_y  = 0.0f;
    cal->gyro_bias_z  = 0.0f;
#if ENABLE_FLEX_SENSORS
    for (uint8_t i = 0; i < FLEX_CHANNEL_COUNT; i++) {
        cal->flex_min[i] = 0;
        cal->flex_max[i] = 0;
    }
#endif
    cal->flags = 0;
}

// ------------------- NVS 加载 / 保存 -------------------
bool LoadCalibration(CalibrationData* out_cal) {
    if (!out_cal) return false;
    ResetCalibrationStruct(out_cal);

    Preferences prefs;
    // 首次烧录时 NVS 中不存在 kNvsNamespace，如果直接以 readOnly 方式打开，
    // Arduino-ESP32 会在串口吐 "nvs: not found" 的 ESP_LOG 警告（框架行为，
    // 非代码 bug）。先用读写模式静默 begin+end 一次，把命名空间创建出来；
    // 再用只读模式正式读取，既能避开噪音日志，又保持后续读操作的最小权限。
    {
        Preferences warmup;
        if (warmup.begin(kNvsNamespace, false)) {
            warmup.end();
        }
    }

    // readOnly = true
    if (!prefs.begin(kNvsNamespace, true)) {
        // 此时仍然失败属硬件 NVS 故障，视为"无校准数据可用"
        return false;
    }

    uint8_t ver = prefs.getUChar(kKeyVersion, 0);
    if (ver != CALIBRATION_NVS_VERSION) {
        // 版本不兼容：视为未校准
        prefs.end();
        return false;
    }

    out_cal->flags = prefs.getUShort(kKeyFlags, 0);

    if (out_cal->flags & CAL_FLAG_IMU_OK) {
        out_cal->accel_bias_x = prefs.getFloat(kKeyAxBias, 0.0f);
        out_cal->accel_bias_y = prefs.getFloat(kKeyAyBias, 0.0f);
        out_cal->accel_bias_z = prefs.getFloat(kKeyAzBias, 0.0f);
        out_cal->gyro_bias_x  = prefs.getFloat(kKeyGxBias, 0.0f);
        out_cal->gyro_bias_y  = prefs.getFloat(kKeyGyBias, 0.0f);
        out_cal->gyro_bias_z  = prefs.getFloat(kKeyGzBias, 0.0f);
    }

#if ENABLE_FLEX_SENSORS
    if (out_cal->flags & CAL_FLAG_FLEX_OK) {
        for (uint8_t i = 0; i < FLEX_CHANNEL_COUNT; i++) {
            out_cal->flex_min[i] = prefs.getUShort(kKeyFlexMin[i], 0);
            out_cal->flex_max[i] = prefs.getUShort(kKeyFlexMax[i], 0);
        }
    }
#endif

    prefs.end();
    return true;
}

bool SaveCalibration(const CalibrationData& cal) {
    Preferences prefs;
    // readOnly = false
    if (!prefs.begin(kNvsNamespace, false)) {
        DEBUG_PRINTLN("[校准] NVS 打开失败 (写入模式)");
        return false;
    }

    bool ok = true;
    ok = prefs.putUChar(kKeyVersion, (uint8_t)CALIBRATION_NVS_VERSION) > 0 && ok;
    ok = prefs.putUShort(kKeyFlags, cal.flags) > 0 && ok;

    if (cal.flags & CAL_FLAG_IMU_OK) {
        ok = prefs.putFloat(kKeyAxBias, cal.accel_bias_x) > 0 && ok;
        ok = prefs.putFloat(kKeyAyBias, cal.accel_bias_y) > 0 && ok;
        ok = prefs.putFloat(kKeyAzBias, cal.accel_bias_z) > 0 && ok;
        ok = prefs.putFloat(kKeyGxBias, cal.gyro_bias_x)  > 0 && ok;
        ok = prefs.putFloat(kKeyGyBias, cal.gyro_bias_y)  > 0 && ok;
        ok = prefs.putFloat(kKeyGzBias, cal.gyro_bias_z)  > 0 && ok;
    }

#if ENABLE_FLEX_SENSORS
    if (cal.flags & CAL_FLAG_FLEX_OK) {
        for (uint8_t i = 0; i < FLEX_CHANNEL_COUNT; i++) {
            ok = prefs.putUShort(kKeyFlexMin[i], cal.flex_min[i]) > 0 && ok;
            ok = prefs.putUShort(kKeyFlexMax[i], cal.flex_max[i]) > 0 && ok;
        }
    }
#endif

    prefs.end();
    return ok;
}

void ClearCalibration() {
    Preferences prefs;
    if (prefs.begin(kNvsNamespace, false)) {
        prefs.clear();
        prefs.end();
    }
}

// ------------------- 应用到 sensor_manager -------------------
void ApplyCalibration(const CalibrationData& cal) {
    if (cal.flags & CAL_FLAG_IMU_OK) {
        setImuBias(cal.accel_bias_x, cal.accel_bias_y, cal.accel_bias_z,
                   cal.gyro_bias_x,  cal.gyro_bias_y,  cal.gyro_bias_z);
    } else {
        // 未校准：显式清零，避免上一次运行的残留生效（即便 setup 保证初值为 0，
        // 连续多次 ApplyCalibration 调用时也稳）
        setImuBias(0, 0, 0, 0, 0, 0);
    }

#if ENABLE_FLEX_SENSORS
    if (cal.flags & CAL_FLAG_FLEX_OK) {
        for (uint8_t i = 0; i < FLEX_CHANNEL_COUNT; i++) {
            setFlexRuntimeRange(i, cal.flex_min[i], cal.flex_max[i]);
        }
    } else {
        // 未校准：回退到 config.h 给的 FLEX_ADC_MIN/MAX 默认值
        for (uint8_t i = 0; i < FLEX_CHANNEL_COUNT; i++) {
            setFlexRuntimeRange(i,
                                (uint16_t)FLEX_ADC_MIN,
                                (uint16_t)FLEX_ADC_MAX);
        }
    }
#endif
}

// ------------------- IMU 零偏校准流程 -------------------
bool RunImuZeroingCalibration(CalibrationData* cal,
                              ReadSampleFn read_sample,
                              uint32_t duration_ms,
                              uint32_t sample_interval_ms) {
    if (!cal || !read_sample) return false;
    if (duration_ms < 500 || sample_interval_ms == 0) {
        DEBUG_PRINTLN("[校准] 参数不合法 (duration_ms<500 或 interval=0)");
        return false;
    }

    const uint32_t t_start = millis();
    const uint32_t t_end   = t_start + duration_ms;
    uint32_t t_next = t_start;

    double sum_ax = 0.0, sum_ay = 0.0, sum_az = 0.0;
    double sum_gx = 0.0, sum_gy = 0.0, sum_gz = 0.0;
    uint32_t n_ok   = 0;
    uint32_t n_fail = 0;

    while (millis() < t_end) {
        if ((int32_t)(millis() - t_next) < 0) {
            delay(2);
            continue;
        }
        t_next += sample_interval_ms;

        float ax, ay, az, gx, gy, gz;
        if (!read_sample(&ax, &ay, &az, &gx, &gy, &gz)) {
            n_fail++;
            if (n_fail >= 3 && n_ok == 0) {
                DEBUG_PRINTLN("[校准] 采样连续失败，中止");
                return false;
            }
            continue;
        }
        sum_ax += ax;  sum_ay += ay;  sum_az += az;
        sum_gx += gx;  sum_gy += gy;  sum_gz += gz;
        n_ok++;
    }

    if (n_ok < 10) {
        DEBUG_LOG("[校准] 有效采样数不足 (%d)，需 ≥10", n_ok);
        return false;
    }

    const double inv_n = 1.0 / (double)n_ok;
    // accel_z 扣除 +1g 重力（约定：手套平放，Z 轴向上）
    cal->accel_bias_x = (float)(sum_ax * inv_n);
    cal->accel_bias_y = (float)(sum_ay * inv_n);
    cal->accel_bias_z = (float)(sum_az * inv_n - 1.0);
    cal->gyro_bias_x  = (float)(sum_gx * inv_n);
    cal->gyro_bias_y  = (float)(sum_gy * inv_n);
    cal->gyro_bias_z  = (float)(sum_gz * inv_n);
    cal->flags |= CAL_FLAG_IMU_OK;

    DEBUG_LOG("[校准] IMU 采样完成: n=%d, fail=%d", n_ok, n_fail);
    return true;
}

#if ENABLE_FLEX_SENSORS
bool RunFlexStageCalibration(uint16_t out_values[FLEX_CHANNEL_COUNT],
                             ReadFlexRawFn read_flex_raw,
                             uint32_t duration_ms,
                             uint32_t sample_interval_ms) {
    if (!out_values || !read_flex_raw) return false;
    if (duration_ms < 500 || sample_interval_ms == 0) return false;

    uint32_t sum[FLEX_CHANNEL_COUNT] = {0};
    uint32_t n_ok   = 0;
    uint32_t n_fail = 0;

    const uint32_t t_start = millis();
    const uint32_t t_end   = t_start + duration_ms;
    uint32_t t_next = t_start;

    while (millis() < t_end) {
        if ((int32_t)(millis() - t_next) < 0) {
            delay(2);
            continue;
        }
        t_next += sample_interval_ms;

        uint16_t raw[FLEX_CHANNEL_COUNT];
        if (!read_flex_raw(raw)) {
            n_fail++;
            if (n_fail >= 3 && n_ok == 0) {
                DEBUG_PRINTLN("[校准] Flex 采样连续失败，中止");
                return false;
            }
            continue;
        }
        for (uint8_t i = 0; i < FLEX_CHANNEL_COUNT; i++) {
            sum[i] += raw[i];
        }
        n_ok++;
    }

    if (n_ok < 10) {
        DEBUG_PRINTLN("[校准] Flex 有效采样数不足 10");
        return false;
    }

    for (uint8_t i = 0; i < FLEX_CHANNEL_COUNT; i++) {
        out_values[i] = (uint16_t)(sum[i] / n_ok);
    }
    return true;
}
#endif

void PrintCalibration(const CalibrationData& cal) {
    DEBUG_PRINTLN("---- CalibrationData ----");
    DEBUG_LOG("  flags = 0x%X", (uint32_t)(cal.flags));

    DEBUG_LOG("  IMU_OK = %s", (cal.flags & CAL_FLAG_IMU_OK) ? "yes" : "no");
    if (cal.flags & CAL_FLAG_IMU_OK) {
        DEBUG_LOG("    accel_bias (g): %.4f, %.4f, %.4f", (double)(cal.accel_bias_x), (double)(cal.accel_bias_y), (double)(cal.accel_bias_z));
        DEBUG_LOG("    gyro_bias (deg/s): %.3f, %.3f, %.3f", (double)(cal.gyro_bias_x), (double)(cal.gyro_bias_y), (double)(cal.gyro_bias_z));
    }

#if ENABLE_FLEX_SENSORS
    DEBUG_LOG("  FLEX_OK = %s", (cal.flags & CAL_FLAG_FLEX_OK) ? "yes" : "no");
    if (cal.flags & CAL_FLAG_FLEX_OK) {
        for (uint8_t i = 0; i < FLEX_CHANNEL_COUNT; i++) {
            DEBUG_LOG("    flex[%d] min=%u max=%u", (int)i, (uint32_t)cal.flex_min[i], (uint32_t)cal.flex_max[i]);
        }
    }
#endif
    DEBUG_PRINTLN("-------------------------");
}
