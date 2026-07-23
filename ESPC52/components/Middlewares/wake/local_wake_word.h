#ifndef LOCAL_WAKE_WORD_H
#define LOCAL_WAKE_WORD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/**
 * @file local_wake_word.h
 * @brief C5 到 S3 唤醒迁移期间的 voice_chain 兼容门。
 */

/* 本地唤醒确认音配置：只影响提示音和提示音后录音窗口延迟。 */
#ifndef LOCAL_WAKE_RECORD_DELAY_AFTER_ACK_MS
#define LOCAL_WAKE_RECORD_DELAY_AFTER_ACK_MS 350U // 提示音播放后等待录音的延迟，单位 ms。
#endif

#ifndef LOCAL_WAKE_ACK_SAMPLE_RATE_HZ
#define LOCAL_WAKE_ACK_SAMPLE_RATE_HZ 16000U // 提示音 PCM 采样率。
#endif

typedef void (*local_wake_prompt_done_cb_t)(uint32_t generation,
                                             esp_err_t result,
                                             uint32_t duration_ms,
                                             void *user_ctx);

/** 调用方法：voice_chain_start() 初始化时调用一次；C5 不加载模型。 */
esp_err_t local_wake_word_init(void);

/** 调用方法：Mic VAD 开始后判断提示音冷却是否结束，true 才允许开始上传录音。 */
bool local_wake_word_should_record_after_vad_start(void);

/** Legacy explicit UI wake request; it does not run a C5 detector. */
esp_err_t local_wake_word_on_local_wake_detected(void);

/**
 * Start the fixed, firmware-embedded "wo zai, ni shuo" PCM prompt.  The
 * caller returns immediately; completion is delivered from the prompt task
 * after the speaker writer has drained the PCM stream.
 */
esp_err_t local_wake_word_start_prompt_async(uint32_t generation);

/** Register the voice-chain completion sink for the local wake prompt. */
void local_wake_word_set_prompt_done_callback(local_wake_prompt_done_cb_t callback,
                                              void *user_ctx);

/**
 * @brief Mic ADC 已重建并开始采样后，正式打开本轮用户录音窗口。
 *
 * 调用方法：只由 voice_chain 在 MIC_RECORD_READY 后调用；提示音时间不计入录音窗口。
 */
esp_err_t local_wake_word_open_recording_window(void);

/** 调用方法：一轮录音完成、服务器开始播放或异常恢复时调用，关闭录音窗口。 */
esp_err_t local_wake_word_on_recording_finished(void);

/** 调用方法：错误恢复时调用，强制关闭录音窗口并清除提示音状态。 */
void local_wake_word_cancel_recording_window(void);

/** 调用方法：voice_chain 判断当前是否处于本地唤醒后的录音窗口。 */
bool local_wake_word_is_recording_window_open(void);

bool local_wake_word_is_ack_active(void);

#endif /* LOCAL_WAKE_WORD_H */
