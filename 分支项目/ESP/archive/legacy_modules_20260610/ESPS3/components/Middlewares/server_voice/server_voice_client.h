#ifndef SERVER_VOICE_CLIENT_H
#define SERVER_VOICE_CLIENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/**
 * @file server_voice_client.h
 * @brief ESP-server 语音 turn 薄客户端。
 *
 * 调用方法：voice_chain 初始化后调用 init()；VAD 触发时调用 prepare/start，
 * 录音过程中 append_pcm()，句尾 finish_turn()，异常路径 cancel_turn()。
 * 本模块只上传 PCM、接收并播放服务器裸 PCM，不调用云端 ASR/TTS，不解析 JSON/base64。
 */

#ifndef SERVER_VOICE_READ_CHUNK_BYTES
#define SERVER_VOICE_READ_CHUNK_BYTES 1024 // 服务器 PCM 响应单次读取字节数。
#endif

#ifndef SERVER_VOICE_RESPONSE_TASK_STACK
#define SERVER_VOICE_RESPONSE_TASK_STACK 12288 // 响应读取/播放任务栈，单位字节。
#endif

#ifndef SERVER_VOICE_RESPONSE_TASK_PRIORITY
#define SERVER_VOICE_RESPONSE_TASK_PRIORITY 4 // 响应读取/播放任务优先级。
#endif

typedef esp_err_t (*server_voice_done_cb_t)(void *user_ctx);
typedef esp_err_t (*server_voice_playback_start_cb_t)(void *user_ctx);
typedef esp_err_t (*server_voice_error_cb_t)(int code, const char *message, void *user_ctx);

typedef struct {
    server_voice_done_cb_t done_cb;
    void *done_ctx;
    server_voice_playback_start_cb_t playback_start_cb;
    void *playback_start_ctx;
    server_voice_error_cb_t error_cb;
    void *error_ctx;
} server_voice_client_config_t;

/** 调用方法：voice_chain_start() 里注册 done/playback/error 回调，重复调用返回 ESP_OK。 */
esp_err_t server_voice_client_init(const server_voice_client_config_t *config);

/** 调用方法：本地唤醒确认后调用，当前只做轻量 ready 检查和日志。 */
esp_err_t server_voice_client_prepare_async(void);

/** 调用方法：VAD 确认进入录音窗口后调用，打开到 ESP-server 的 chunked PCM POST。 */
esp_err_t server_voice_client_start_turn(void);

/** 调用方法：Mic 每累计一个 PCM 小块后调用，samples 是 int16_t 样本数。 */
esp_err_t server_voice_client_append_pcm(const int16_t *pcm, size_t samples);

/** 调用方法：VAD 结束并发送完 post-roll 后调用，结束上传并异步读取服务器 PCM。 */
esp_err_t server_voice_client_finish_turn(void);

/** 调用方法：voice_chain 错误恢复或退出当前 turn 时调用，可重复调用。 */
esp_err_t server_voice_client_cancel_turn(void);

/** 调用方法：Mic/VAD 判断是否能开启下一轮 server voice turn 时调用。 */
bool server_voice_client_is_idle(void);

/** 调用方法：voice_chain 需要判断语音客户端是否占用中时调用。 */
bool server_voice_client_is_active(void);

#endif /* SERVER_VOICE_CLIENT_H */
