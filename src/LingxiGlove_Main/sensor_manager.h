// ============================================================
// sensor_manager.h
// 传感器管理模块 - 封装 MPU6050 与弯曲传感器的数据采集
// ============================================================
// 采集通道：
//   - MPU6050 (I2C)         : 始终启用，6 轴原始数据 + pitch/roll 姿态角
//   - 5 路弯曲传感器 (ADC)   : 由 config.h 的 ENABLE_FLEX_SENSORS 条件编译控制
//                             关闭时 data.flexValid=false；启用时完整采样与归一化
// ============================================================

#ifndef SENSOR_MANAGER_H
#define SENSOR_MANAGER_H

#include <Arduino.h>
#include "config.h"   // 提供 FLEX_CHANNEL_COUNT、ENABLE_FLEX_SENSORS、FLEX_ADC_MIN/MAX 等

// ------------------- MPU6050 I2C 配置 -------------------
#define MPU6050_ADDR        0x68    // AD0接GND时的I2C地址
#define MPU6050_REG_PWR_MGMT_1  0x6B
#define MPU6050_REG_ACCEL_XOUT  0x3B
#define MPU6050_REG_GYRO_XOUT   0x43
#define MPU6050_REG_CONFIG      0x1A
#define MPU6050_REG_GYRO_CONFIG 0x1B
#define MPU6050_REG_ACCEL_CONFIG 0x1C

// 弯曲传感器手指索引（通道数 FLEX_CHANNEL_COUNT 在 config.h 中定义）
enum FlexFinger {
    FLEX_THUMB = 0,
    FLEX_INDEX = 1,
    FLEX_MIDDLE = 2,
    FLEX_RING = 3,
    FLEX_PINKY = 4
};

// ------------------- 传感器数据结构 -------------------
struct SensorData {
    // MPU6050 原始数据（已转换为物理单位）
    float accelX;   // g
    float accelY;   // g
    float accelZ;   // g
    float gyroX;    // deg/s
    float gyroY;    // deg/s
    float gyroZ;    // deg/s

    // 解算姿态角（基于加速度计）
    float pitch;    // 俯仰角，手掌上下倾斜（+ = 朝上）
    float roll;     // 横滚角，手掌左右倾斜（+ = 左倾）

    // 弯曲传感器原始 ADC 读数 (0~4095)
    // ENABLE_FLEX_SENSORS=0 时 flexValid=false, flex[] 全为 0
    uint16_t flex[FLEX_CHANNEL_COUNT];
    // 归一化弯曲度 [0.0, 1.0]：0=伸直 1=完全弯曲（基于 FLEX_ADC_MIN/MAX 线性映射）
    float    flexNorm[FLEX_CHANNEL_COUNT];
    bool     flexValid;

    // 状态标志
    bool mpuValid;

    // 时间戳
    unsigned long timestamp;
};

// 初始化所有传感器（MVP阶段仅初始化 MPU6050）
bool initSensors();

// 读取所有传感器数据
bool readSensors(SensorData& data);

// 打印传感器数据到串口（调试用）
void printSensorData(const SensorData& data);

// 计算姿态角（内部函数，供readSensors调用）
void computeOrientation(SensorData& data);

// ------------------- 运行时校准注入接口 -------------------
// 由 calibration 模块在启动时/重新校准后调用，sensor_manager 保存偏移值，
// 并在 readSensors 的物理单位转换之后、姿态角计算之前应用。
// 单位：加速度 bias 为 g，陀螺仪 bias 为 deg/s。
// 约定：accel_bias_z 已扣除 1.0g 重力（即纯偏移），见 calibration.h。
// 未调用此函数时，sensor_manager 的偏移初值全部为 0（等价于"未校准"）。
void setImuBias(float ax_bias, float ay_bias, float az_bias,
                float gx_bias, float gy_bias, float gz_bias);

// 设置第 channel 路弯曲传感器的运行时归一化量程（替代 config.h 的 FLEX_ADC_MIN/MAX）。
// 仅在 ENABLE_FLEX_SENSORS=1 时有意义；关闭时本函数为空实现（便于无条件调用）。
// channel 越界或 min_val+8 >= max_val 时直接忽略，避免 calibration 传入非法值导致除零。
void setFlexRuntimeRange(uint8_t channel, uint16_t min_val, uint16_t max_val);

#endif // SENSOR_MANAGER_H
