// ============================================================
// offline_voice_pcm.cpp
// 离线语音 PCM 表的定义文件（默认为空；由 gen_offline_voice_pcm.py 覆盖）
// ------------------------------------------------------------
// 实现约定：
//   1. 本 .cpp 的存在是为了把"默认空表"集中在此，避免把非 const
//      数组或大型 PCM 数据堆进头文件导致 ODR / 多次链接。
//   2. 严格 ISO C++ 不允许零长度数组（GCC 以 -Wzero-length-array 扩展提示），
//      所以这里放一个 sentinel 元素 {nullptr, nullptr, 0, 0}，但显式将
//      kOfflinePcmCount 硬编码为 0。使用方 local_tts_fallback.cpp
//      会按 count 做遍历边界，sentinel 永远不会被访问到。
//   3. sentinel 的 label/data 都是 nullptr，即便有人错误地把 count 设成 1，
//      local_tts_fallback 里的 `if (!e.label || !e.data) continue;` 也会
//      把它防御性跳过——严格保证"空表状态下不可能产生播报"。
//   4. 生成脚本 tools/gen_offline_voice_pcm.py 在有真实数据时会覆盖
//      整个文件，包括 sizeof/sizeof 写法的 kOfflinePcmCount，届时 sentinel
//      不再存在；两种布局通过 local_tts_fallback 的防御式代码都兼容。
// ============================================================

#include "offline_voice_pcm.h"

// Sentinel-only 表：放一条永远不可能命中（label/data 均为 nullptr）的占位项，
// 避开 ISO C++ "zero size array" 扩展；真实数据由脚本生成时会完全替换掉。
const OfflinePcmEntry kOfflinePcmTable[] = {
    { nullptr, nullptr, 0u, 0u },
};

// 显式写 0 —— 让使用方认为当前是空表；
// 若未来有人填充真实 label，请同步把本常量改为 sizeof(...) / sizeof(...[0])
// 或让 gen_offline_voice_pcm.py 全量重写本文件。
const size_t kOfflinePcmCount = 0u;
