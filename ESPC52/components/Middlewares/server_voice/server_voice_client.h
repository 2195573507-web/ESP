#ifndef SERVER_VOICE_CLIENT_H
#define SERVER_VOICE_CLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/**
 * @file server_voice_client.h
 * @brief C5 终端访问 ESPS3 voice_proxy 的语音 turn 客户端。
 *
 * 调用方法：voice_chain 初始化后调用 init()；VAD 触发时调用 prepare/start，
 * 录音过程中 append_pcm()，句尾 finish_turn()，异常路径 cancel_turn()。
 * 本模块只上传 PCM 到 S3、接收并播放 S3 回传的裸 PCM，不直连公网服务，不实现 ASR/LLM/TTS。
 */

#ifndef SERVER_VOICE_READ_CHUNK_BYTES
#define SERVER_VOICE_READ_CHUNK_BYTES 1024 // 服务器 PCM 响应单次读取字节数。
#endif

#ifndef SERVER_VOICE_RESPONSE_TASK_STACK
#define SERVER_VOICE_RESPONSE_TASK_STACK 8192 // 响应读取/播放任务栈，单位字节。
#endif

#ifndef SERVER_VOICE_RESPONSE_TASK_PRIORITY
#define SERVER_VOICE_RESPONSE_TASK_PRIORITY 4 // 响应读取/播放任务优先级。
#endif

typedef esp_err_t (*server_voice_done_cb_t)(uint32_t lease_generation, void *user_ctx);
typedef esp_err_t (*server_voice_playback_start_cb_t)(void *user_ctx);
typedef esp_err_t (*server_voice_error_cb_t)(uint32_t lease_generation,
                                              int code,
                                              const char *message,
                                              void *user_ctx);

typedef struct {
    server_voice_done_cb_t done_cb;
    void *done_ctx;
    server_voice_playback_start_cb_t playback_start_cb;
    void *playback_start_ctx;
    server_voice_error_cb_t error_cb;
    void *error_ctx;
} server_voice_client_config_t;

/**
 * @brief 初始化 voice 客户端并注册回调。
 *
 * 调用位置：voice_chain_start()。
 * @param config 回调配置，可为空；非空时保存 done/playback/error 回调。
 * @return ESP_OK 表示初始化完成；参数当前不触发失败。
 * 失败处理：voice_chain 若收到非 ESP_OK 会中止启动并记录错误。
 */
esp_err_t server_voice_client_init(const server_voice_client_config_t *config);

/** Update the gateway reachability admission state for future voice turns. */
void server_voice_client_set_network_available(bool available, const char *reason);

/** Re-establish local voice-turn admission after NETWORK_READY. */
esp_err_t server_voice_client_reconnect(void);

/** @brief 本地唤醒确认后准备一轮 turn；当前做 ready 检查和日志，失败由 voice_chain 中止本轮。 */
esp_err_t server_voice_client_prepare_async(void);

/**
 * Associate the next /local/v1/voice/turn request with the S3-issued command
 * capture.  Both values are sent in request headers so S3 can reject stale or
 * cross-session PCM without relying on the UDP WakeNet stream id.
 */
esp_err_t server_voice_client_set_command_capture_metadata(const char *command_id,
                                                           uint32_t command_stream_id,
                                                           uint32_t command_generation);

/** @brief VAD 进入录音窗口后打开到 ESPS3 的 PCM POST；失败由 voice_chain 释放本轮资源。 */
esp_err_t server_voice_client_start_turn(void);

/** @brief 追加一段 Mic PCM；pcm 不能为空，samples 为 int16_t 样本数，失败由 voice_chain 取消本轮。 */
esp_err_t server_voice_client_append_pcm(const int16_t *pcm, size_t samples);

/** @brief VAD 结束后完成上传并异步读取 S3 回传 PCM；失败由 voice_chain 进入错误恢复。 */
esp_err_t server_voice_client_finish_turn(void);

/** @brief 取消当前 turn 并释放 HTTP/上传缓存；错误恢复路径调用，可重复调用。 */
esp_err_t server_voice_client_cancel_turn(void);

/** @brief 断联中止当前 turn；正在读响应时请求任务自行退出并释放 HTTP/speaker 资源。 */
esp_err_t server_voice_client_request_abort(const char *reason);

/** @brief Wait for the response receiver to leave the current session. */
esp_err_t server_voice_client_wait_for_idle(uint32_t timeout_ms);

/** @brief 判断客户端是否空闲；Mic/VAD 开启下一轮 turn 前调用。 */
bool server_voice_client_is_idle(void);

/** @brief 判断客户端是否占用中；voice_chain 状态判断使用。 */
bool server_voice_client_is_active(void);

#endif /* SERVER_VOICE_CLIENT_H */
