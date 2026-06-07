// ============================================================
// sensor_manager.cpp
// 传感器管理模块实现
// ============================================================

#include "sensor_manager.h"
#include "config.h"
#include <Wire.h>

// MPU6050 灵敏度系数
#define ACCEL_SENSITIVITY   16384.0f  // ±2g 量程
#define GYRO_SENSITIVITY    131.0f    // ±250°/s 量程

static bool s_sensorsInitialized = false;

// ------------------- 运行时校准偏移（IMU） -------------------
// 默认全 0，等价于"未校准但可直出裸数据"；由 setImuBias() 注入。
// accel 单位 g、gyro 单位 deg/s；accel_z 的偏移已扣除 +1g 重力（见头文件约定）。
static float s_accel_bias_x = 0.0f;
static float s_accel_bias_y = 0.0f;
static float s_accel_bias_z = 0.0f;
static float s_gyro_bias_x  = 0.0f;
static float s_gyro_bias_y  = 0.0f;
static float s_gyro_bias_z  = 0.0f;

#if ENABLE_FLEX_SENSORS
// 5 路弯曲传感器 ADC 引脚映射（与 FlexFinger 枚举严格对应，顺序不可变）。
// 默认使用 MASTER（右手）映射；setFlexPinMapping(g_runtime_role) 在 setup() 中
// 根据 NVS 角色覆写为 SLAVE 映射。两只手套烧同一固件，运行期切换。
static uint8_t s_flex_pins[FLEX_CHANNEL_COUNT] = FLEX_PINS_MASTER;

// 运行时归一化量程：默认使用 config.h 的 FLEX_ADC_MIN/MAX，
// 由 setFlexRuntimeRange() 在校准加载后覆写。
static uint16_t s_flex_min[FLEX_CHANNEL_COUNT] = {
    (uint16_t)FLEX_ADC_MIN, (uint16_t)FLEX_ADC_MIN, (uint16_t)FLEX_ADC_MIN,
    (uint16_t)FLEX_ADC_MIN, (uint16_t)FLEX_ADC_MIN
};
static uint16_t s_flex_max[FLEX_CHANNEL_COUNT] = {
    (uint16_t)FLEX_ADC_MAX, (uint16_t)FLEX_ADC_MAX, (uint16_t)FLEX_ADC_MAX,
    (uint16_t)FLEX_ADC_MAX, (uint16_t)FLEX_ADC_MAX
};
#endif

// ------------------- 运行时校准注入接口实现 -------------------
void setImuBias(float ax_bias, float ay_bias, float az_bias,
                float gx_bias, float gy_bias, float gz_bias) {
    s_accel_bias_x = ax_bias;
    s_accel_bias_y = ay_bias;
    s_accel_bias_z = az_bias;
    s_gyro_bias_x  = gx_bias;
    s_gyro_bias_y  = gy_bias;
    s_gyro_bias_z  = gz_bias;
}

void setFlexRuntimeRange(uint8_t channel, uint16_t min_val, uint16_t max_val) {
#if ENABLE_FLEX_SENSORS
    if (channel >= FLEX_CHANNEL_COUNT) return;
    // 至少需要 8 个 ADC 码的量程才认为合法（12-bit ADC 的 ~0.2%），
    // 避免除零 / 信噪比过低导致归一化结果毫无意义。
    if ((uint32_t)max_val <= (uint32_t)min_val + 8u) return;
    s_flex_min[channel] = min_val;
    s_flex_max[channel] = max_val;
#else
    // ENABLE_FLEX_SENSORS=0 时该接口是空实现，方便上层无条件调用
    (void)channel; (void)min_val; (void)max_val;
#endif
}

void setFlexPinMapping(uint8_t role) {
#if ENABLE_FLEX_SENSORS
    if (role == 0) {
        // MASTER = 右手
        const uint8_t master_pins[FLEX_CHANNEL_COUNT] = FLEX_PINS_MASTER;
        memcpy(s_flex_pins, master_pins, sizeof(master_pins));
    } else if (role == 1) {
        // SLAVE = 左手
        const uint8_t slave_pins[FLEX_CHANNEL_COUNT] = FLEX_PINS_SLAVE;
        memcpy(s_flex_pins, slave_pins, sizeof(slave_pins));
    }
    // 其它非 0/1 值忽略，保持当前映射不变
#else
    (void)role;
#endif
}

// ---------------------------------------------------------
// 内部函数：读取 5 路弯曲传感器
// ENABLE_FLEX_SENSORS=1: 对每路做 FLEX_ADC_OVERSAMPLE 次过采样取均值，
//                       再用 FLEX_ADC_MIN/MAX 线性归一化到 [0,1] 并钳位，data.flexValid=true
// ENABLE_FLEX_SENSORS=0: 全部置零，data.flexValid=false（硬件未到货 / 未接线时）
// ---------------------------------------------------------
static void readFlexSensors(SensorData& data) {
#if ENABLE_FLEX_SENSORS
    for (uint8_t ch = 0; ch < FLEX_CHANNEL_COUNT; ch++) {
        // 多次采样平均，降低白噪声
        uint32_t acc = 0;
        for (uint8_t i = 0; i < FLEX_ADC_OVERSAMPLE; i++) {
            acc += analogRead(s_flex_pins[ch]);
        }
        uint16_t raw = (uint16_t)(acc / FLEX_ADC_OVERSAMPLE);
        data.flex[ch] = raw;

        // 使用运行时量程（默认 = config.h 的 FLEX_ADC_MIN/MAX，
        // 个体校准加载后被 setFlexRuntimeRange 覆写）
        const uint16_t ch_min = s_flex_min[ch];
        const uint16_t ch_max = s_flex_max[ch];
        const float span = (float)ch_max - (float)ch_min;

        // 归一化到 [0.0, 1.0]：伸直=0，完全弯曲=1
        float norm = 0.0f;
        if (span > 1.0f) {
            float v = (float)raw - (float)ch_min;
            norm = v / span;
            if (norm < 0.0f) norm = 0.0f;
            if (norm > 1.0f) norm = 1.0f;
        }
        data.flexNorm[ch] = norm;
    }
    data.flexValid = true;
#else
    for (uint8_t ch = 0; ch < FLEX_CHANNEL_COUNT; ch++) {
        data.flex[ch] = 0;
        data.flexNorm[ch] = 0.0f;
    }
    data.flexValid = false;
#endif
}

// ---------------------------------------------------------
// 内部函数：向 MPU6050 写寄存器
// ---------------------------------------------------------
static void mpuWriteByte(uint8_t reg, uint8_t data) {
    Wire.beginTransmission(MPU6050_ADDR);
    Wire.write(reg);
    Wire.write(data);
    Wire.endTransmission();
}

// ---------------------------------------------------------
// 内部函数：从 MPU6050 读寄存器
// ---------------------------------------------------------
static bool mpuReadBytes(uint8_t reg, uint8_t* buffer, uint8_t len) {
    Wire.beginTransmission(MPU6050_ADDR);
    Wire.write(reg);
    uint8_t err = Wire.endTransmission(false);
    if (err != 0) return false;

    // 显式 (uint8_t,uint8_t) 消除与 (int,int) / (uint16_t,uint8_t) 重载的歧义
    uint8_t n = Wire.requestFrom((uint8_t)MPU6050_ADDR, (uint8_t)len);
    if (n != len) return false;

    for (uint8_t i = 0; i < len; i++) {
        buffer[i] = Wire.read();
    }
    return true;
}

// ---------------------------------------------------------
// 初始化传感器
// ---------------------------------------------------------
bool initSensors() {
    if (s_sensorsInitialized) return true;

    DEBUG_PRINTLN("[Sensor] 初始化传感器...");

    // 初始化 I2C
    Wire.begin();
    Wire.setClock(400000);  // 400kHz 快速模式

    // 唤醒 MPU6050
    mpuWriteByte(MPU6050_REG_PWR_MGMT_1, 0x00);
    delay(100);

    // 验证芯片ID（可选）
    uint8_t whoami = 0;
    if (!mpuReadBytes(0x75, &whoami, 1)) {
        DEBUG_PRINTLN("[Sensor] MPU6050 I2C 通信失败");
        return false;
    }
    if (whoami != 0x68) {
        DEBUG_LOG("[Sensor] MPU6050 ID 异常: 0x%X", (uint32_t)whoami);
        // 某些兼容模块返回0x72或其他值，继续尝试
    }

    // 配置低通滤波器 (DLPF) = 3 (~44Hz 带宽，降低噪声)
    mpuWriteByte(MPU6050_REG_CONFIG, 0x03);

    // 配置陀螺仪量程 ±250°/s
    mpuWriteByte(MPU6050_REG_GYRO_CONFIG, 0x00);

    // 配置加速度计量程 ±2g
    mpuWriteByte(MPU6050_REG_ACCEL_CONFIG, 0x00);

#if ENABLE_FLEX_SENSORS
    // 配置 ADC 分辨率为 12-bit（ESP32-S3 原生支持），默认衰减已足够覆盖 0~3.3V
    analogReadResolution(12);
    for (uint8_t ch = 0; ch < FLEX_CHANNEL_COUNT; ch++) {
        pinMode(s_flex_pins[ch], INPUT);
    }
    DEBUG_PRINTLN("[Sensor] 弯曲传感器 ADC 通道已初始化 (5 路)");
#else
    DEBUG_PRINTLN("[Sensor] 弯曲传感器未启用 (ENABLE_FLEX_SENSORS=0)");
#endif

    s_sensorsInitialized = true;
    DEBUG_PRINTLN("[Sensor] MPU6050 初始化成功");
    return true;
}

// ---------------------------------------------------------
// 读取传感器数据
// ---------------------------------------------------------
bool readSensors(SensorData& data) {
    if (!s_sensorsInitialized) return false;

    uint8_t buffer[14];
    if (!mpuReadBytes(MPU6050_REG_ACCEL_XOUT, buffer, 14)) {
        data.mpuValid = false;
        return false;
    }

    // 解析加速度计原始数据（大端序）
    int16_t rawAccelX = (buffer[0] << 8) | buffer[1];
    int16_t rawAccelY = (buffer[2] << 8) | buffer[3];
    int16_t rawAccelZ = (buffer[4] << 8) | buffer[5];

    // 跳过温度 (buffer[6:7])

    // 解析陀螺仪原始数据
    int16_t rawGyroX  = (buffer[8]  << 8) | buffer[9];
    int16_t rawGyroY  = (buffer[10] << 8) | buffer[11];
    int16_t rawGyroZ  = (buffer[12] << 8) | buffer[13];

    // 转换为物理单位
    data.accelX = rawAccelX / ACCEL_SENSITIVITY;
    data.accelY = rawAccelY / ACCEL_SENSITIVITY;
    data.accelZ = rawAccelZ / ACCEL_SENSITIVITY;

    data.gyroX = rawGyroX / GYRO_SENSITIVITY;
    data.gyroY = rawGyroY / GYRO_SENSITIVITY;
    data.gyroZ = rawGyroZ / GYRO_SENSITIVITY;

    // 应用个体校准偏移（未校准时全 0 = 无影响）
    // 必须在 computeOrientation 之前，这样 pitch/roll 也反映校准结果
    data.accelX -= s_accel_bias_x;
    data.accelY -= s_accel_bias_y;
    data.accelZ -= s_accel_bias_z;
    data.gyroX  -= s_gyro_bias_x;
    data.gyroY  -= s_gyro_bias_y;
    data.gyroZ  -= s_gyro_bias_z;

    // 计算姿态角
    computeOrientation(data);

    // 弯曲传感器读取（由 ENABLE_FLEX_SENSORS 开关控制）
    readFlexSensors(data);

    data.mpuValid = true;
    data.timestamp = millis();
    return true;
}

// ---------------------------------------------------------
// 计算姿态角（基于加速度计）
// ---------------------------------------------------------
void computeOrientation(SensorData& data) {
    // 使用 atan2 计算 pitch 和 roll
    // pitch: 绕X轴旋转（手掌上下翻转）
    // roll: 绕Y轴旋转（手掌左右倾斜）
    float ax = data.accelX;
    float ay = data.accelY;
    float az = data.accelZ;

    // pitch = atan2(x, sqrt(y^2 + z^2))
    data.pitch = atan2(ax, sqrt(ay * ay + az * az)) * 180.0f / PI;

    // roll = atan2(y, z)
    data.roll = atan2(ay, az) * 180.0f / PI;
}

// ---------------------------------------------------------
// 打印传感器数据（调试用）
// ---------------------------------------------------------
void printSensorData(const SensorData& data) {
    if (!data.mpuValid) {
        DEBUG_PRINTLN("[Sensor] 数据无效");
        return;
    }

    DEBUG_LOG("[Sensor] Accel(g): %.2f, %.2f, %.2f | Gyro(d/s): %.1f, %.1f, %.1f | Pitch: %.1f | Roll: %.1f",
              (double)data.accelX, (double)data.accelY, (double)data.accelZ,
              (double)data.gyroX,  (double)data.gyroY,  (double)data.gyroZ,
              (double)data.pitch,  (double)data.roll);
    if (data.flexValid) {
        // Flex 逐通道打印：裸 ADC + 归一化值，便于硬件验证
        static const char* s_flexNames[FLEX_CHANNEL_COUNT] = {
            "Thumb", "Index", "Middle", "Ring", "Pinky"
        };
        for (uint8_t ch = 0; ch < FLEX_CHANNEL_COUNT; ch++) {
            DEBUG_LOG("  Flex[%d %s]: raw=%u norm=%.2f",
                      (int)ch, s_flexNames[ch],
                      (unsigned)data.flex[ch],
                      (double)data.flexNorm[ch]);
        }
    }
}
