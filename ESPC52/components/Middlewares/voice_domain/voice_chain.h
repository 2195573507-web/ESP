#ifndef VOICE_CHAIN_H
#define VOICE_CHAIN_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifndef VOICE_COMMAND_SESSION_ID_MAX_LEN
#define VOICE_COMMAND_SESSION_ID_MAX_LEN 64U
#endif

typedef enum {
    VOICE_COMMAND_SESSION_NONE = 0,
    VOICE_COMMAND_SESSION_CREATE,
    VOICE_COMMAND_SESSION_WAIT_ACK,
    VOICE_COMMAND_SESSION_CAPTURE_READY,
    VOICE_COMMAND_SESSION_CAPTURING,
    VOICE_COMMAND_SESSION_UPLOAD,
    VOICE_COMMAND_SESSION_PROCESSING,
    VOICE_COMMAND_SESSION_DONE,
    VOICE_COMMAND_SESSION_FAILED,
} voice_command_session_state_t;

/* The command identity is immutable after CREATE.  A nonzero wake_stream_id
 * binds the S3 wake event to this C52 command session; command_stream_id is
 * assigned only when Mic command capture is armed. */
typedef struct VoiceCommandSession {
    char command_id[VOICE_COMMAND_SESSION_ID_MAX_LEN];
    uint32_t generation;
    uint32_t wake_stream_id;
    uint32_t command_stream_id;
    voice_command_session_state_t state;
    int64_t timestamp;
} VoiceCommandSession;

/**
 * @file voice_chain.h
 * @brief C5 终端半双工 voice turn 编排接口。
 *
 * app_orchestrator_start() 在 WiFi/BME/system 准备后调用 voice_chain_start()。
 * 本层只编排 Mic/VAD、wake ack、非语音暂停、PCM 上传/播放和 Mic 恢复；不在 C5 固件中
 * 实现 ASR/LLM/TTS。
 */

/* 语音链路任务配置：只调整本地状态机资源，不改变服务器协议。 */
#ifndef VOICE_CHAIN_QUEUE_DEPTH
#define VOICE_CHAIN_QUEUE_DEPTH 4U // voice_chain 内部事件队列深度。
#endif

#ifndef VOICE_CHAIN_TASK_STACK
#define VOICE_CHAIN_TASK_STACK 4096U // PSRAM task stack; high_water~7952 on 8 KiB; retain headroom.
#endif

#ifndef VOICE_CHAIN_TASK_PRIORITY
#define VOICE_CHAIN_TASK_PRIORITY 4U // voice_chain 任务优先级。
#endif

#ifndef VOICE_MIC_PAUSE_TIMEOUT_MS
#define VOICE_MIC_PAUSE_TIMEOUT_MS 2000U // 等待 Mic ADC 进入暂停态的超时，单位 ms。
#endif

typedef enum {
    VOICE_IDLE = 0,
    VOICE_WAKE_LISTENING,
    VOICE_WAKE_DETECTED,
    VOICE_ACK_PLAYBACK,
    VOICE_COMMAND_CAPTURE,
    VOICE_COMMAND_UPLOAD,
    VOICE_RESPONSE_PLAYBACK,
    VOICE_RECOVERY,
} voice_chain_state_t;

/* Compatibility names for existing C5 consumers.  New transitions and logs
 * always use the explicit pipeline states above. */
#define VOICE_LISTENING        VOICE_WAKE_LISTENING
#define VOICE_WAKE_ACK         VOICE_ACK_PLAYBACK
#define VOICE_RECORDING        VOICE_COMMAND_CAPTURE
#define VOICE_WAITING_RESPONSE VOICE_COMMAND_UPLOAD
#define VOICE_PLAYING          VOICE_RESPONSE_PLAYBACK
#define VOICE_ERROR            VOICE_RECOVERY

/**
 * @brief 启动 C5 半双工语音链路。
 *
 * 调用位置：app_orchestrator_start()。
 * 调用时机：WiFi 稳定、system_service/BME service 启动后。
 * 输入参数：无。
 * @return ESP_OK 表示已启动或已在运行；初始化 wake/audio/server_voice/Mic 或任务创建失败返回对应错误码。
 * 失败处理：orchestrator 记录错误；其他后台链路继续运行，下一次重启再尝试。
 */
esp_err_t voice_chain_start(void);

/**
 * Notify the voice task that the S3 gateway has entered or left NETWORK_READY.
 * The call only queues a state transition; it never performs HTTP or Mic work
 * in the caller's context.  A lost link aborts an in-flight turn, while a
 * recovered link re-arms the remote PCM pipeline without restarting C5 VAD.
 */
esp_err_t voice_chain_notify_network_state(bool available, const char *reason);

/**
 * Queue a user-initiated local wake request for the voice task.  This is the
 * only LCD-facing entry point: UI/touch code never changes voice state or
 * allocates audio resources directly.
 */
esp_err_t voice_chain_request_local_wake(void);

/**
 * Record a command session before the C52 ACK is sent.  This only creates the
 * local session and transitions it to WAIT_ACK; it never starts Mic capture.
 */
esp_err_t voice_chain_create_command_session(const char *command_id,
                                             uint32_t command_generation,
                                             uint32_t wake_stream_id,
                                             uint32_t command_timeout_ms);

/**
 * Queue command capture only after /local/v1/commands/{id}/ack succeeded.
 * The complete command identity must match the existing WAIT_ACK session.
 */
esp_err_t voice_chain_request_command_capture(const char *command_id,
                                              uint32_t command_generation,
                                              uint32_t wake_stream_id,
                                              uint32_t command_timeout_ms);

/** Mark a pre-capture session failed when its ACK cannot be delivered. */
void voice_chain_fail_command_session(const char *command_id,
                                      uint32_t command_generation,
                                      const char *reason);

/** @brief 获取当前 voice_chain 状态；状态页、日志或调试命令调用，返回枚举值。 */
voice_chain_state_t voice_chain_get_state(void);

/** @brief 把 voice_chain_state_t 转为静态字符串；日志调用，不需要释放。 */
const char *voice_chain_state_name(voice_chain_state_t state);

#endif /* VOICE_CHAIN_H */
