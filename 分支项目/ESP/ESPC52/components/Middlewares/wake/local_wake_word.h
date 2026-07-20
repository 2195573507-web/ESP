#ifndef LOCAL_WAKE_WORD_H
#define LOCAL_WAKE_WORD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/**
 * @file local_wake_word.h
 * @brief voice_chain 使用的本地唤醒词抽象。
 *
 * 本模块负责固定唤醒词“你好小智”的离线 WakeNet 检测。空闲时 Server 保持静默，
 * 只有 WakeNet 命中后才打开本地提示音和录音窗口，再进入现有 server voice chain。
 */

/* 本地唤醒确认音配置：只影响提示音和提示音后录音窗口延迟。 */
#ifndef LOCAL_WAKE_RECORD_DELAY_AFTER_ACK_MS
#define LOCAL_WAKE_RECORD_DELAY_AFTER_ACK_MS 350U // 提示音播放后等待录音的延迟，单位 ms。
#endif

#ifndef LOCAL_WAKE_ACK_SAMPLE_RATE_HZ
#define LOCAL_WAKE_ACK_SAMPLE_RATE_HZ 16000U // 提示音 PCM 采样率。
#endif

/** 调用方法：voice_chain_start() 初始化时调用一次，加载 WakeNet 模型。 */
esp_err_t local_wake_word_init(void);

/** 调用方法：Mic 任务分配 WakeNet 输入缓冲时调用，返回每次 detect 需要的样本数。 */
size_t local_wake_word_get_chunk_samples(void);

/** 调用方法：Mic 任务判断是否可以向本地 WakeNet 喂 PCM。 */
bool local_wake_word_is_detection_ready(void);

/** 调用方法：Mic 空闲态每累计一块 16 kHz int16 PCM 后调用。 */
esp_err_t local_wake_word_detect_chunk(const int16_t *pcm, size_t samples, bool *detected);

/** 调用方法：Mic 暂停/恢复或异常恢复时调用，只清除本模块的检测锁存状态。 */
void local_wake_word_reset_detector(void);

/** 调用方法：Mic VAD 开始后判断提示音冷却是否结束，true 才允许开始上传录音。 */
bool local_wake_word_should_record_after_vad_start(void);

/** 调用方法：本地 WakeNet 触发后调用，会请求 S3 当前 wake prompt；失败则短 beep。 */
esp_err_t local_wake_word_on_local_wake_detected(void);

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

/** 调用方法：voice_chain 判断提示音是否仍在播放，播放中不要开始录音。 */
bool local_wake_word_is_ack_active(void);

#endif /* LOCAL_WAKE_WORD_H */
