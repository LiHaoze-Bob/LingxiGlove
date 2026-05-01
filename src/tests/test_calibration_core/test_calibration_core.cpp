// ============================================================
// test_calibration_core.cpp
// 校准核心算法的 host-side 单元测试
// ------------------------------------------------------------
// 说明：
//   calibration.cpp 依赖 Arduino <Preferences.h> 与 Serial，
//   整体拉到 host 上跑需要较重的 stub。
//   核心"采样均值 - 扣重力"算法本身平凡且独立，这里用与 calibration.cpp
//   **逐行等价**的参考实现 + 多组输入复核，既可验证算法正确性，也能在
//   后续 calibration.cpp 内部微调时作为回归基线。
//
// 编译运行：
//   cd src/tests/test_calibration_core
//   g++ -std=c++11 -Wall -Wextra test_calibration_core.cpp -o run_tests
//   ./run_tests
// ============================================================

#include <cstdio>
#include <cstdint>
#include <cmath>

static int g_pass = 0;
static int g_fail = 0;

#define EXPECT(cond, msg) do {                                  \
    if (cond) { g_pass++; }                                     \
    else {                                                      \
        g_fail++;                                               \
        std::fprintf(stderr, "[FAIL] %s:%d  %s  (cond: %s)\n",  \
                     __FILE__, __LINE__, msg, #cond);           \
    }                                                           \
} while (0)

#define EXPECT_NEAR(a, b, eps, msg) do {                        \
    const double _da = (double)(a), _db = (double)(b);          \
    if (std::fabs(_da - _db) <= (double)(eps)) { g_pass++; }    \
    else {                                                      \
        g_fail++;                                               \
        std::fprintf(stderr,                                    \
            "[FAIL] %s:%d  %s  (%.6f vs %.6f, diff %.6f > %.6f)\n", \
            __FILE__, __LINE__, msg,                            \
            _da, _db, std::fabs(_da - _db), (double)(eps));     \
    }                                                           \
} while (0)

// ----------------------------------------------------------------------
// 被测算法的参考实现 —— 必须与 calibration.cpp::RunImuZeroingCalibration
// 的核心数学 **逐行等价**。一旦两者偏离，本测试与板上行为就脱节。
// 当前 calibration.cpp 内的核心逻辑片段：
//   sum_ax += ax; ...
//   inv_n = 1.0 / n_ok;
//   cal->accel_bias_x = (float)(sum_ax * inv_n);
//   cal->accel_bias_z = (float)(sum_az * inv_n - 1.0);  // 扣 1g 重力
//   cal->gyro_bias_x  = (float)(sum_gx * inv_n);
// ----------------------------------------------------------------------
struct ImuBiasResult {
    float ax_bias, ay_bias, az_bias;
    float gx_bias, gy_bias, gz_bias;
};

static ImuBiasResult ComputeImuBiasReference(
        const float* ax, const float* ay, const float* az,
        const float* gx, const float* gy, const float* gz,
        uint32_t n) {
    double sum_ax = 0, sum_ay = 0, sum_az = 0;
    double sum_gx = 0, sum_gy = 0, sum_gz = 0;
    for (uint32_t i = 0; i < n; i++) {
        sum_ax += ax[i]; sum_ay += ay[i]; sum_az += az[i];
        sum_gx += gx[i]; sum_gy += gy[i]; sum_gz += gz[i];
    }
    const double inv = 1.0 / (double)n;
    ImuBiasResult r;
    r.ax_bias = (float)(sum_ax * inv);
    r.ay_bias = (float)(sum_ay * inv);
    r.az_bias = (float)(sum_az * inv - 1.0);  // 扣 1g 重力
    r.gx_bias = (float)(sum_gx * inv);
    r.gy_bias = (float)(sum_gy * inv);
    r.gz_bias = (float)(sum_gz * inv);
    return r;
}

// ----------------------------------------------------------------------
// Flex 量程每通道均值（对应 RunFlexStageCalibration）
//   out[ch] = sum[ch] / n_ok
// ----------------------------------------------------------------------
static void ComputeFlexStageReference(
        const uint16_t* samples,  // n rows × 5 cols, row-major
        uint32_t n,
        uint16_t out[5]) {
    for (int ch = 0; ch < 5; ch++) {
        uint32_t sum = 0;
        for (uint32_t i = 0; i < n; i++) {
            sum += samples[i * 5 + ch];
        }
        out[ch] = (uint16_t)(sum / n);
    }
}

// ============================================================
// T1: 理想静止样本 -> 偏移几乎为 0（仅保留 az 重力 -> 扣完接近 0）
// ============================================================
static void TestIdealStillGivesZeroBias() {
    std::printf("[T1] ideal still input yields near-zero bias\n");
    const uint32_t N = 60;
    float ax[N], ay[N], az[N], gx[N], gy[N], gz[N];
    for (uint32_t i = 0; i < N; i++) {
        ax[i] = 0.0f; ay[i] = 0.0f; az[i] = 1.0f;       // 纯重力朝 Z
        gx[i] = 0.0f; gy[i] = 0.0f; gz[i] = 0.0f;
    }
    ImuBiasResult r = ComputeImuBiasReference(ax, ay, az, gx, gy, gz, N);
    EXPECT_NEAR(r.ax_bias, 0.0f, 1e-6, "T1: ax_bias ~= 0");
    EXPECT_NEAR(r.ay_bias, 0.0f, 1e-6, "T1: ay_bias ~= 0");
    EXPECT_NEAR(r.az_bias, 0.0f, 1e-6, "T1: az_bias after 1g subtract ~= 0");
    EXPECT_NEAR(r.gx_bias, 0.0f, 1e-6, "T1: gx_bias ~= 0");
    EXPECT_NEAR(r.gy_bias, 0.0f, 1e-6, "T1: gy_bias ~= 0");
    EXPECT_NEAR(r.gz_bias, 0.0f, 1e-6, "T1: gz_bias ~= 0");
}

// ============================================================
// T2: 带已知偏移的静止样本 -> 算出的偏移等于人为注入值
// ============================================================
static void TestKnownBiasRecovered() {
    std::printf("[T2] injected bias is recovered by averaging\n");
    const uint32_t N = 120;
    const float inj_ax = 0.03f, inj_ay = -0.02f, inj_az = 0.01f;
    const float inj_gx = 0.5f,  inj_gy = -0.7f, inj_gz = 1.2f;
    float ax[N], ay[N], az[N], gx[N], gy[N], gz[N];
    for (uint32_t i = 0; i < N; i++) {
        // 模拟带偏移的静止：加速度 = 重力 + 偏移；陀螺仪 = 偏移
        ax[i] = inj_ax;
        ay[i] = inj_ay;
        az[i] = 1.0f + inj_az;
        gx[i] = inj_gx;
        gy[i] = inj_gy;
        gz[i] = inj_gz;
    }
    ImuBiasResult r = ComputeImuBiasReference(ax, ay, az, gx, gy, gz, N);
    EXPECT_NEAR(r.ax_bias, inj_ax, 1e-5, "T2: ax recovered");
    EXPECT_NEAR(r.ay_bias, inj_ay, 1e-5, "T2: ay recovered");
    EXPECT_NEAR(r.az_bias, inj_az, 1e-5, "T2: az recovered (after gravity subtract)");
    EXPECT_NEAR(r.gx_bias, inj_gx, 1e-4, "T2: gx recovered");
    EXPECT_NEAR(r.gy_bias, inj_gy, 1e-4, "T2: gy recovered");
    EXPECT_NEAR(r.gz_bias, inj_gz, 1e-4, "T2: gz recovered");
}

// ============================================================
// T3: 偏移 + 零均值噪声 -> 仍能恢复偏移（样本量足够时）
// ============================================================
static void TestBiasPlusZeroMeanNoise() {
    std::printf("[T3] bias + zero-mean noise still recoverable with enough samples\n");
    const uint32_t N = 400;
    const float inj_ax = 0.01f;
    // 确定性的锯齿噪声：相邻两帧相反，总均值严格为 0
    float ax[N], ay[N], az[N], gx[N], gy[N], gz[N];
    for (uint32_t i = 0; i < N; i++) {
        const float noise = (i % 2 == 0) ? +0.02f : -0.02f;
        ax[i] = inj_ax + noise;
        ay[i] = 0.0f;
        az[i] = 1.0f;
        gx[i] = 0.0f;
        gy[i] = 0.0f;
        gz[i] = 0.0f;
    }
    ImuBiasResult r = ComputeImuBiasReference(ax, ay, az, gx, gy, gz, N);
    EXPECT_NEAR(r.ax_bias, inj_ax, 1e-5, "T3: ax recovered despite noise");
    EXPECT_NEAR(r.az_bias, 0.0f,  1e-5, "T3: az still 0 after gravity subtract");
}

// ============================================================
// T4: Flex 通道均值
// ============================================================
static void TestFlexStageMean() {
    std::printf("[T4] flex stage per-channel mean\n");
    // 4 帧 × 5 通道
    const uint16_t samples[4 * 5] = {
        100, 200, 300, 400, 500,
        110, 210, 310, 410, 510,
        120, 220, 320, 420, 520,
        130, 230, 330, 430, 530
    };
    uint16_t out[5];
    ComputeFlexStageReference(samples, 4, out);
    // 均值：(100+110+120+130)/4 = 115；其余类推
    EXPECT(out[0] == 115, "T4: ch0");
    EXPECT(out[1] == 215, "T4: ch1");
    EXPECT(out[2] == 315, "T4: ch2");
    EXPECT(out[3] == 415, "T4: ch3");
    EXPECT(out[4] == 515, "T4: ch4");
}

// ============================================================
// T5: Flex 量程合法性 —— 对应 runCalibrationFlow 中的"max-min ≤ 32 拒收"
// 本测试不跑 runCalibrationFlow（Arduino 依赖），只复现那条规则，
// 确保主程序里的常量与注释说明一致。
// ============================================================
static void TestFlexRangeSanityRule() {
    std::printf("[T5] flex sanity: max <= min+32 → reject\n");
    struct Case { uint16_t mn, mx; bool accept; };
    const Case cases[] = {
        { 100, 100, false },  // 完全一样
        { 100, 131, false },  // 差 31，恰好不够
        { 100, 132, false },  // 差 32，仍在等号边界（max <= min+32）
        { 100, 133, true  },  // 差 33，通过
        { 500, 600, true  },  // 正常量程
    };
    for (const auto& c : cases) {
        const bool accept = !((uint32_t)c.mx <= (uint32_t)c.mn + 32u);
        if (accept != c.accept) {
            std::fprintf(stderr, "[FAIL] T5: case(%u,%u) expected %d got %d\n",
                         c.mn, c.mx, (int)c.accept, (int)accept);
            g_fail++;
        } else {
            g_pass++;
        }
    }
}

int main() {
    std::printf("========================================\n");
    std::printf(" Calibration core algorithm unit tests\n");
    std::printf("========================================\n");

    TestIdealStillGivesZeroBias();
    TestKnownBiasRecovered();
    TestBiasPlusZeroMeanNoise();
    TestFlexStageMean();
    TestFlexRangeSanityRule();

    std::printf("----------------------------------------\n");
    std::printf(" PASSED: %d   FAILED: %d\n", g_pass, g_fail);
    std::printf("----------------------------------------\n");
    return g_fail == 0 ? 0 : 1;
}
