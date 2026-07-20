#ifndef APP_RUNTIME_H
#define APP_RUNTIME_H

#include <stdbool.h>

#include "esp_err.h"

/**
 * @file app_runtime.h
 * @brief 语音独占期间的非语音模块运行时 gate。
 *
 * 调用方法：voice_chain 在 VAD 检测到说话后调用 pause_non_voice()，在服务器 PCM
 * 播放和语音资源释放完成后调用 resume_non_voice()。本模块不暂停 WiFi、Mic/VAD、
 * server_voice_client 或 Speaker，只 gate BME690、上传、UI/非必要网络等非语音入口。
 */

/* 非语音暂停配置：语音链路等待 BME/上传等后台模块让出资源的最大时间。 */
#ifndef APP_RUNTIME_NON_VOICE_PAUSE_TIMEOUT_MS
#define APP_RUNTIME_NON_VOICE_PAUSE_TIMEOUT_MS 6000U // pause_non_voice 等待超时，单位 ms。
#endif

/** 调用方法：VAD 检测到本地唤醒、语音 turn 开始前调用；reason 用于日志。 */
esp_err_t app_runtime_pause_non_voice(const char *reason);

/** 调用方法：服务器播放结束并释放语音资源后调用；会恢复 BME/非语音服务。 */
esp_err_t app_runtime_resume_non_voice(const char *reason);

/** 调用方法：状态页或调试日志查询当前非语音模块是否被语音链路 gate。 */
bool app_runtime_non_voice_is_paused(void);

#endif // APP_RUNTIME_H
