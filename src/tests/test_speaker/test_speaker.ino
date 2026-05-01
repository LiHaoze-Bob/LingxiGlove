/**
 * LingxiGlove 人声语音播放测试程序
 *
 * 播放预生成的TTS语音"你好，我是灵犀手套"
 * 语音由微软Edge-TTS生成，转换为16kHz/16bit/单声道PCM
 *
 * 接线 (Keyestudio扩展板右侧红色区域 S/V/G):
 *   S列[最内侧]=信号  V列[中间]=电源  G列[最外侧]=地线
 *   I2S模块 BCLK -> S列第1个 (D4/GPIO7)
 *   I2S模块 LRC  -> S列第2个 (D5/GPIO8)
 *   I2S模块 DIN  -> S列第3个 (D6/GPIO9)
 *   I2S模块 VCC  -> V列任意一个 (3.3V或5V)
 *   I2S模块 GND  -> G列任意一个 (GND)
 *   I2S模块 SD   -> G列任意一个 (GND=右声道)
 *   喇叭         -> I2S模块绿色接线柱
 */

#include <driver/i2s.h>
#include "voice_pcm.h"   // TTS语音数据: "你好，我是灵犀手套"

// ========== 引脚配置 ==========
#define I2S_BCLK  7   // D4
#define I2S_LRC   8   // D5
#define I2S_DIN   9   // D6

// ========== I2S配置 ==========
#define SAMPLE_RATE     16000

// ========== 函数声明 ==========
void initI2S();
void playPCM(const uint8_t* data, size_t len);

void setup() {
    Serial.begin(115200);
    while (!Serial) { ; }
    delay(500);

    Serial.println("\n================================");
    Serial.println("  LingxiGlove 人声语音播放测试");
    Serial.println("================================");
    Serial.print("语音内容: 你好，我是灵犀手套\n");
    Serial.print("PCM大小: ");
    Serial.print(AUDIO_PCM_LEN);
    Serial.println(" bytes");
    Serial.print("预估时长: ");
    Serial.print(AUDIO_PCM_LEN / 2 / SAMPLE_RATE);
    Serial.println(" seconds\n");

    initI2S();

    // 播放第一遍
    Serial.println("[播放] 第一遍...");
    playPCM(AUDIO_PCM, AUDIO_PCM_LEN);
    delay(1000);

    // 播放第二遍
    Serial.println("[播放] 第二遍...");
    playPCM(AUDIO_PCM, AUDIO_PCM_LEN);

    Serial.println("\n================================");
    Serial.println("  进入循环播放模式 (每5秒一次)");
    Serial.println("================================\n");
}

void loop() {
    delay(5000);  // 间隔5秒
    Serial.println("[循环播放] 你好，我是灵犀手套");
    playPCM(AUDIO_PCM, AUDIO_PCM_LEN);
}

// ========== I2S初始化 ==========
void initI2S() {
    i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
        .sample_rate = SAMPLE_RATE,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8,
        .dma_buf_len = 512,
        .use_apll = false,
        .tx_desc_auto_clear = true,
        .fixed_mclk = 0
    };

    i2s_pin_config_t pin_config = {
        .bck_io_num = I2S_BCLK,
        .ws_io_num = I2S_LRC,
        .data_out_num = I2S_DIN,
        .data_in_num = I2S_PIN_NO_CHANGE
    };

    esp_err_t err = i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
    if (err != ESP_OK) {
        Serial.println("[错误] I2S驱动安装失败!");
        while (1) { delay(1000); }
    }

    err = i2s_set_pin(I2S_NUM_0, &pin_config);
    if (err != ESP_OK) {
        Serial.println("[错误] I2S引脚配置失败!");
        while (1) { delay(1000); }
    }

    Serial.println("[OK] I2S初始化成功 (16kHz, 16bit, Mono)\n");
}

// ========== 播放PCM数据 ==========
// data: PCM字节数组 (16bit, little-endian, mono)
// len:  字节数
void playPCM(const uint8_t* data, size_t len) {
    if (!data || len == 0) return;

    // 分块写入I2S，避免一次性占用太多内存
    const size_t CHUNK_SIZE = 1024;
    size_t bytesWritten = 0;
    size_t offset = 0;

    while (offset < len) {
        size_t toWrite = min(CHUNK_SIZE, len - offset);
        i2s_write(I2S_NUM_0, data + offset, toWrite, &bytesWritten, portMAX_DELAY);
        offset += toWrite;
    }

    // 等待DMA缓冲区排空
    delay(200);
}
