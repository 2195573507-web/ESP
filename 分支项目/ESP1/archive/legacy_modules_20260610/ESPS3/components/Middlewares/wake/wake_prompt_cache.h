#ifndef WAKE_PROMPT_CACHE_H
#define WAKE_PROMPT_CACHE_H

#include "esp_err.h"

/**
 * @file wake_prompt_cache.h
 * @brief Wake prompt TTS PCM flash cache.
 *
 * This module downloads the "我在，你说" prompt after Wi-Fi is ready, stores it
 * in a small writable SPIFFS partition, and lets the wake path play it in
 * chunks before falling back to the embedded prompt.
 */

#ifndef WAKE_PROMPT_CACHE_DOWNLOAD_TIMEOUT_MS
#define WAKE_PROMPT_CACHE_DOWNLOAD_TIMEOUT_MS 5000U
#endif

#ifndef WAKE_PROMPT_CACHE_TASK_STACK
#define WAKE_PROMPT_CACHE_TASK_STACK 8192U
#endif

#ifndef WAKE_PROMPT_CACHE_TASK_PRIORITY
#define WAKE_PROMPT_CACHE_TASK_PRIORITY 3U
#endif

/** 调用方法：WiFi stable 后调用一次；函数只启动低优先级后台任务，不阻塞启动。 */
esp_err_t wake_prompt_cache_start_async(void);

/** 调用方法：本地唤醒提示音播放阶段调用；成功播放缓存，失败返回原因给内置音回退。 */
esp_err_t wake_prompt_cache_play(void);

#endif /* WAKE_PROMPT_CACHE_H */
