#ifndef VOICE_CHAIN_H
#define VOICE_CHAIN_H

#include "esp_err.h"

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
    VOICE_LISTENING,
    VOICE_WAKE_ACK,
    VOICE_RECORDING,
    VOICE_WAITING_RESPONSE,
    VOICE_PLAYING,
    VOICE_ERROR,
} voice_chain_state_t;

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
 * Queue a user-initiated local wake request for the voice task.  This is the
 * only LCD-facing entry point: UI/touch code never changes voice state or
 * allocates audio resources directly.
 */
esp_err_t voice_chain_request_local_wake(void);

/** @brief 获取当前 voice_chain 状态；状态页、日志或调试命令调用，返回枚举值。 */
voice_chain_state_t voice_chain_get_state(void);

/** @brief 把 voice_chain_state_t 转为静态字符串；日志调用，不需要释放。 */
const char *voice_chain_state_name(voice_chain_state_t state);

#endif /* VOICE_CHAIN_H */
