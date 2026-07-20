#include "speaker_resample.h"

/**
 * @file speaker_resample.c
 * @brief 16 kHz -> 24 kHz PCM 线性重采样实现。
 *
 * 只服务 speaker/server voice 语音播报路径：不引入额外 DSP 依赖，按 2:3 的位置关系
 * 做线性插值，把服务器常见 16 kHz PCM 转成 IIS/PDM 需要的 24 kHz PCM。
 */

#include <limits.h>

size_t audio_resample_16k_to_24k_output_samples(size_t input_samples)
{
    /* 16 kHz -> 24 kHz 等价于采样点数量乘 3/2，向上取整保留最后一个输入点。 */
    if (input_samples > (SIZE_MAX - 1U) / 3U) {
        return 0;
    }
    return (input_samples * 3U + 1U) / 2U;
}

esp_err_t audio_resample_16k_to_24k_linear(const int16_t *input,
                                           size_t input_samples,
                                           int16_t *output,
                                           size_t output_capacity,
                                           size_t *output_samples)
{
    if (output_samples != NULL) {
        *output_samples = 0;
    }
    if (input == NULL || output == NULL || output_samples == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (input_samples == 0) {
        return ESP_OK;
    }

    const size_t required_samples = audio_resample_16k_to_24k_output_samples(input_samples);
    if (required_samples == 0) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (output_capacity < required_samples) {
        return ESP_ERR_INVALID_SIZE;
    }

    for (size_t out_index = 0; out_index < required_samples; out_index++) {
        /*
         * 输出采样点映射回输入时间轴：
         * out_index / 24000 = in_position / 16000，所以 in_position = out_index * 2 / 3。
         */
        const size_t pos_num = out_index * 2U;
        const size_t in_index = pos_num / 3U;
        const size_t frac = pos_num % 3U;
        const int32_t s0 = input[in_index];
        const int32_t s1 = (in_index + 1U < input_samples) ? input[in_index + 1U] : s0;
        int32_t sample = s0;

        if (frac == 1U) {
            sample = (2 * s0 + s1) / 3;
        } else if (frac == 2U) {
            sample = (s0 + 2 * s1) / 3;
        }

        if (sample > INT16_MAX) {
            sample = INT16_MAX;
        } else if (sample < INT16_MIN) {
            sample = INT16_MIN;
        }
        output[out_index] = (int16_t)sample;
    }

    *output_samples = required_samples;
    return ESP_OK;
}
