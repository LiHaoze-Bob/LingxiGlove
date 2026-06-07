// ============================================================
// calibration.h
// 个体校准模块（IMU 零偏 + 弯曲传感器量程校准）
// ------------------------------------------------------------
// 设计原则：
//   1. 数据在 NVS（Preferences 命名空间 "lingxi_cal"）中持久化，
//      断电重启后自动加载，避免每次使用都要重校准。
//   2. 只存"已实测并确认"的校准值。未校准时 flags=0，默认值全零，
//      绝不伪造假设数据。
//   3. 不 include sensor_manager.h，避免与 sensor_manager 形成循环依赖。
//      采集样本通过回调函数指针 ReadSampleFn 注入，由主程序提供。
//
// 典型使用：
//   CalibrationData cal;
//   LoadCalibration(&cal);                 // 启动时
//   ApplyCalibration(cal);                 // 写回 sensor_manager
//   ...
//   // 用户按 'k'：
//   RunImuZeroingCalibration(&cal, ReadSampleAdapter, 3000);
//   SaveCalibration(cal);
//   ApplyCalibration(cal);
// ============================================================

#ifndef CALIBRATION_H
#define CALIBRATION_H

#include <stdint.h>
#include "config.h"   // 提供 FLEX_CHANNEL_COUNT、ENABLE_FLEX_SENSORS、FLEX_ADC_MIN/MAX

// 本版本号写入 NVS；改动存储布局时需 +1，老数据自动作废
#define CALIBRATION_NVS_VERSION  1

// flags 位掩码
#define CAL_FLAG_IMU_OK          (1u << 0)
#define CAL_FLAG_FLEX_OK         (1u << 1)

struct CalibrationData {
    // IMU 零偏（单位与 sensor_manager 的 SensorData 一致：accel=g, gyro=deg/s）
    // 约定：accel_bias_z 已经扣除 1.0g 重力，校准后 (data.accelZ - accel_bias_z)
    // 在静止平放时应接近 +1.0g。
    float accel_bias_x;
    float accel_bias_y;
    float accel_bias_z;
    float gyro_bias_x;
    float gyro_bias_y;
    float gyro_bias_z;

#if ENABLE_FLEX_SENSORS
    // 弯曲传感器运行时量程（替代 config.h 中的 FLEX_ADC_MIN/MAX 硬编码）
    // flex_min[i]：第 i 路手指完全伸直时的 ADC 值
    // flex_max[i]：第 i 路手指完全弯曲时的 ADC 值
    uint16_t flex_min[FLEX_CHANNEL_COUNT];
    uint16_t flex_max[FLEX_CHANNEL_COUNT];
#endif

    uint16_t flags;   // 位掩码：哪些校准已完成
};

// 只读取一帧传感器样本的回调。返回 true 表示采样成功。
// 输出参数 ax,ay,az 单位 g；gx,gy,gz 单位 deg/s。
typedef bool (*ReadSampleFn)(float* ax, float* ay, float* az,
                             float* gx, float* gy, float* gz);

#if ENABLE_FLEX_SENSORS
// 读取一帧弯曲传感器的原始 ADC 值的回调。返回 true 表示成功。
// 输出参数 out_flex 为长度 FLEX_CHANNEL_COUNT 的数组。
typedef bool (*ReadFlexRawFn)(uint16_t out_flex[FLEX_CHANNEL_COUNT]);
#endif

/**
 * @brief 把 CalibrationData 清零（flags=0，所有 bias=0）。
 *        flex_min/flex_max 清零——注意 0 不是合法量程，代表"未校准"；
 *        使用前必须检查 flags & CAL_FLAG_FLEX_OK。
 */
void ResetCalibrationStruct(CalibrationData* cal);

/**
 * @brief 从 NVS 加载校准。若命名空间不存在或版本不匹配，out_cal 被 Reset，返回 false。
 */
bool LoadCalibration(CalibrationData* out_cal);

/**
 * @brief 把校准写入 NVS。返回是否全部字段写成功。
 */
bool SaveCalibration(const CalibrationData& cal);

/**
 * @brief 清除 NVS 中的校准数据。
 */
void ClearCalibration();

/**
 * @brief 把当前已生效的校准应用到 sensor_manager。
 *        只应用 flags 标记为"已校准"的部分；未校准项保持 sensor_manager 的
 *        默认值（IMU bias=0、flex 量程=FLEX_ADC_MIN/MAX）。
 */
void ApplyCalibration(const CalibrationData& cal);

/**
 * @brief IMU 零偏校准：要求用户把手套平放静止，采样 duration_ms 内的
 *        加速度/陀螺仪均值作为零偏。accel_z 额外扣除 1.0g 重力。
 *        采样周期由 sample_interval_ms 给出（典型 50ms 对应 20Hz）。
 *
 * 注意：本函数会阻塞 ~duration_ms + 用户提示时间。调用方负责在调用前
 *       通过 Serial 告知用户保持姿态、并在调用后打印校准结果。
 *
 * @param cal               传入的校准结构体；成功时其 accel/gyro bias
 *                          字段被更新，CAL_FLAG_IMU_OK 被置位。
 * @param read_sample       采样回调。若连续 3 次返回 false，视为硬件故障。
 * @param duration_ms       采样时长（毫秒），建议 ≥ 2000
 * @param sample_interval_ms 采样周期（毫秒），建议 50
 * @return true 表示成功；false 表示采样失败，cal 未被修改。
 */
bool RunImuZeroingCalibration(CalibrationData* cal,
                              ReadSampleFn read_sample,
                              uint32_t duration_ms,
                              uint32_t sample_interval_ms);

#if ENABLE_FLEX_SENSORS
/**
 * @brief 弯曲传感器量程校准的一个阶段：在 duration_ms 内采 ADC 均值。
 *
 * 典型流程：调用两次，第一次采 min（手指伸直），第二次采 max（握拳）。
 * 两次都完成后由调用方合并并置位 CAL_FLAG_FLEX_OK。
 *
 * @param out_values     长度 FLEX_CHANNEL_COUNT 的输出数组，存放采样均值。
 */
bool RunFlexStageCalibration(uint16_t out_values[FLEX_CHANNEL_COUNT],
                             ReadFlexRawFn read_flex_raw,
                             uint32_t duration_ms,
                             uint32_t sample_interval_ms);
#endif

/**
 * @brief 把 CalibrationData 打印到 Serial（供调试）。
 */
void PrintCalibration(const CalibrationData& cal);

/**
 * @brief 把 CalibrationData 以"机器可读紧凑单行"格式打印到 Serial，供上位机
 *        （LingxiCapture）解析。格式：
 *
 *   [CAL_INFO] flags=<u16> ax=<f> ay=<f> az=<f> gx=<f> gy=<f> gz=<f>
 *              fmin=<u>,<u>,<u>,<u>,<u> fmax=<u>,<u>,<u>,<u>,<u>
 *
 * 即便 flags=0（未校准），所有字段也会以 0/默认值出现，保证上位机解析器始终能拿到完整列。
 * 启用 ENABLE_FLEX_SENSORS=0 时省略 fmin/fmax 两列。
 */
void PrintCalibrationMachineReadable(const CalibrationData& cal);

#endif  // CALIBRATION_H
