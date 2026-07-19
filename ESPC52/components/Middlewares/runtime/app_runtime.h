#ifndef APP_RUNTIME_H
#define APP_RUNTIME_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/**
 * @file app_runtime.h
 * @brief 语音独占期间的非语音模块运行时 gate。
 *
 * 调用方法：voice_chain 在 VAD 检测到说话后调用 pause_non_voice()，在服务器 PCM
 * 播放和语音资源释放完成后调用 resume_non_voice()。本模块不暂停 WiFi、WakeNet/VAD、
 * Mic、server_voice_client 或 Speaker，只 gate heartbeat、command poll、BME690、
 * 普通 local_gateway_comm 请求和其他非必要上传。
 */

/* 非语音暂停配置：语音链路等待 BME/上传等后台模块让出资源的最大时间。 */
#ifndef APP_RUNTIME_NON_VOICE_PAUSE_TIMEOUT_MS
#define APP_RUNTIME_NON_VOICE_PAUSE_TIMEOUT_MS 6000U // pause_non_voice 等待超时，单位 ms。
#endif

#ifndef APP_RUNTIME_VOICE_BUSY_LOG_INTERVAL_MS
#define APP_RUNTIME_VOICE_BUSY_LOG_INTERVAL_MS 30000U // 语音独占跳过普通任务的 info 限频。
#endif

/**
 * @brief 暂停非语音后台链路。
 *
 * 调用位置：voice_chain 在进入 server voice turn 前调用。
 * @param reason 日志原因，可为空。
 * @return ESP_OK 表示 BME 等非语音链路已暂停或未运行；等待暂停超时返回对应错误码。
 * 失败处理：voice_chain 记录错误并按当前状态机中止或恢复本轮语音。
 */
esp_err_t app_runtime_pause_non_voice(const char *reason);

/**
 * @brief Pause normal HTTP and BME with separate bounded admission and BME waits.
 *
 * The resource manager uses this lower-level form to preserve the required
 * HTTP -> BME quiesce order without changing legacy callers.
 */
esp_err_t app_runtime_pause_non_voice_timed(const char *reason,
                                            uint32_t http_timeout_ms,
                                            uint32_t bme_timeout_ms);

/**
 * @brief 恢复非语音后台链路。
 *
 * 调用位置：voice_chain 在 Server PCM 播放结束或错误清理后调用。
 * @param reason 日志原因，可为空。
 * @return ESP_OK；当前恢复路径不向上抛出硬件错误。
 */
esp_err_t app_runtime_resume_non_voice(const char *reason);

/**
 * @brief Resume only BME while retaining the normal-HTTP admission gate.
 *
 * Resource release uses this before background workers resume, so no newly
 * scheduled HTTP request can overlap audio cleanup.
 */
esp_err_t app_runtime_resume_non_voice_bme(const char *reason);

/**
 * @brief Reopen normal HTTP admission after every audio resource is released.
 *
 * This is the final step of the ordered C5 resource-manager resume sequence.
 */
esp_err_t app_runtime_finish_non_voice_resume(const char *reason);

/** @brief 查询非语音链路是否处于暂停 gate；状态页或调试日志调用。 */
bool app_runtime_non_voice_is_paused(void);

/** @brief 判断当前是否应跳过普通 HTTP/后台任务；语音 HTTP 调用不要使用此 gate。 */
bool app_runtime_should_skip_non_voice_task(const char *task_name);

/** @brief 对语音独占跳过日志做 30 秒限频；调用方可在本地任务循环中复用。 */
void app_runtime_log_voice_busy_skip(const char *task_name);

#endif // APP_RUNTIME_H
