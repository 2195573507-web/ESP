#ifndef VOICE_CHAIN_H
#define VOICE_CHAIN_H

#include "esp_err.h"

/**
 * @file voice_chain.h
 * @brief Server-only half-duplex voice turn orchestrator.
 *
 * app_main calls voice_chain_start() after Wi-Fi is stable. This layer keeps
 * Mic/VAD capture, local wake acknowledgement, non-voice runtime gating,
 * server_voice_client upload/playback, and Mic recovery in one place.
 * The ESP firmware does not call cloud speech/model/synthesis services here.
 */

/* 语音链路任务配置：只调整本地状态机资源，不改变服务器协议。 */
#ifndef VOICE_CHAIN_QUEUE_DEPTH
#define VOICE_CHAIN_QUEUE_DEPTH 4U // voice_chain 内部事件队列深度。
#endif

#ifndef VOICE_CHAIN_TASK_STACK
#define VOICE_CHAIN_TASK_STACK 8192U // voice_chain 任务栈，单位字节。
#endif

#ifndef VOICE_CHAIN_TASK_PRIORITY
#define VOICE_CHAIN_TASK_PRIORITY 4U // voice_chain 任务优先级。
#endif

#ifndef VOICE_MIC_PAUSE_TIMEOUT_MS
#define VOICE_MIC_PAUSE_TIMEOUT_MS 2000U // 等待 Mic ADC 进入暂停态的超时，单位 ms。
#endif

typedef enum {
    VOICE_IDLE = 0,
    VOICE_LISTENING,
    VOICE_WAKE_ACK,
    VOICE_RECORDING,
    VOICE_WAITING_RESPONSE,
    VOICE_PLAYING,
    VOICE_ERROR,
} voice_chain_state_t;

/** 调用方法：Wi-Fi 稳定、时间同步和 BME service 启动后调用一次；重复调用返回 ESP_OK。 */
esp_err_t voice_chain_start(void);

/** 调用方法：状态页、日志或调试命令读取当前半双工语音状态。 */
voice_chain_state_t voice_chain_get_state(void);

/** 调用方法：打印 voice_chain_state_t 时调用，返回静态字符串，不需要释放。 */
const char *voice_chain_state_name(voice_chain_state_t state);

#endif /* VOICE_CHAIN_H */
