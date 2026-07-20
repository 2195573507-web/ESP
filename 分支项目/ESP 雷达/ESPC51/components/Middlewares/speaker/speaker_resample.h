#ifndef SPEAKER_RESAMPLE_H
#define SPEAKER_RESAMPLE_H

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @file speaker_resample.h
 * @brief speaker PCM 简单重采样工具。
 *
 * 当前只覆盖服务器语音常见的 16 kHz -> speaker/IIS 需要的 24 kHz。
 * 算法为轻量线性插值，适合语音播报路径，不承担通用高保真重采样职责。
 */

/* 服务器语音响应常见输出采样率。 */
#define AUDIO_RESAMPLE_16K_HZ 16000

/* speaker/IIS 播放链路要求的采样率。 */
#define AUDIO_RESAMPLE_24K_HZ 24000

/**
 * @brief 计算 16 kHz PCM 转 24 kHz 后的输出采样点数量。
 *
 * 调用方法：分配或检查输出缓冲区容量前调用。
 *
 * @param input_samples 输入 int16_t 采样点数量。
 * @return 输出 int16_t 采样点数量；输入为 0 时返回 0。
 */
size_t audio_resample_16k_to_24k_output_samples(size_t input_samples);

/**
 * @brief 将 PCM16 mono 16 kHz 线性重采样为 PCM16 mono 24 kHz。
 *
 * 调用方法：server voice 返回 16 kHz PCM 时由 speaker_player 调用。
 *
 * @param input 输入采样数组。
 * @param input_samples 输入采样点数量。
 * @param output 输出缓冲区。
 * @param output_capacity 输出缓冲区可容纳的采样点数量。
 * @param output_samples 实际生成的采样点数量。
 */
esp_err_t audio_resample_16k_to_24k_linear(const int16_t *input,
                                           size_t input_samples,
                                           int16_t *output,
                                           size_t output_capacity,
                                           size_t *output_samples);

#ifdef __cplusplus
}
#endif

#endif /* SPEAKER_RESAMPLE_H */
