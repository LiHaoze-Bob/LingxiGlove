// ============================================================
// LingxiGlove 敏感配置模板（可提交到 Git）
// ============================================================
// 本文件是**模板**，用来告诉其他开发者需要填哪些秘钥。
// 首次克隆仓库后，请执行：
//     cp secrets.example.h secrets.h
// 然后在 secrets.h 中填入真实的 WiFi / API Key 等值。
// secrets.h 已被 .gitignore，不会提交到仓库，从而避免私密信息泄露。
// ============================================================

#ifndef SECRETS_H
#define SECRETS_H

// ------------------- WiFi -------------------
// 2.4 GHz Wi-Fi（ESP32-S3 不支持 5 GHz）
#define WIFI_SSID       "YOUR_WIFI_SSID"
#define WIFI_PASSWORD   "YOUR_WIFI_PASSWORD"

// ------------------- 阿里 DashScope（Qwen-LLM + Qwen-TTS 复用此 Key） -------------------
// 申请地址（北京主站）：https://bailian.console.aliyun.com/
// 注意：北京主站与新加坡国际站的 endpoint 不同，若使用国际站需同时调整
//       config.h 中 QWEN_*_ENDPOINT 为 dashscope-intl.aliyuncs.com
#define QWEN_API_KEY    "YOUR_QWEN_API_KEY"

// ------------------- 百度智能云（可选，仅 LLM_PROVIDER_BAIDU 时使用） -------------------
// 申请地址：https://console.bce.baidu.com/qianfan/
// 如果你只用 Qwen（默认），下面两项保留占位即可，不影响编译。
#define BAIDU_API_KEY      "YOUR_BAIDU_API_KEY"
#define BAIDU_SECRET_KEY   "YOUR_BAIDU_SECRET_KEY"

#endif // SECRETS_H
