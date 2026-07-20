#ifndef MIC_ADC_PCM_H
#define MIC_ADC_PCM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @file mic_adc_pcm.h
 * @brief 独立的 Mic ADC raw 到 PCM 转换模块。
 *
 * 只依赖 C 标准类型；上层传入 ADC raw，即可得到 int16_t PCM。
 * 后续要调的参数集中放在本文件。
 */

/* PCM 固定格式：16000 Hz、单声道、int16_t、little-endian。 */
#define MIC_ADC_PCM_SAMPLE_RATE_HZ        16000 // PCM 采样率，单位 Hz。
#define MIC_ADC_PCM_CHANNEL_COUNT         1     // PCM 声道数，1 表示单声道。
#define MIC_ADC_PCM_BITS_PER_SAMPLE       16    // PCM 位深，固定 int16_t。
#define MIC_ADC_PCM_BYTES_PER_SAMPLE      2     // 每个 PCM 样本占 2 字节。
#define MIC_ADC_PCM_LITTLE_ENDIAN         1     // 1 表示低字节在前。

/* ADC 转 PCM 参数：后续调音量、偏置、位宽时优先改这里。 */
#define MIC_ADC_PCM_ADC_RESOLUTION_BITS   12    // ADC raw 有效位数。
#define MIC_ADC_PCM_ADC_RAW_MAX           ((1U << MIC_ADC_PCM_ADC_RESOLUTION_BITS) - 1U) // ADC raw 最大值。
#define MIC_ADC_PCM_DEFAULT_DC_OFFSET_RAW ((1U << (MIC_ADC_PCM_ADC_RESOLUTION_BITS - 1)) - 1U) // 默认中心点。
#define MIC_ADC_PCM_AUTO_SEED_DC_OFFSET   1     // 1 表示首个样本自动定中心。
#define MIC_ADC_PCM_ENABLE_DC_TRACK       1     // 1 表示运行时慢速跟踪中心点。
#define MIC_ADC_PCM_DC_TRACK_SHIFT        8     // 越大跟踪越慢，低频保留越多。
#define MIC_ADC_PCM_GAIN                  8     // PCM 增益，8 表示 raw 差值放大 8 倍。

#if MIC_ADC_PCM_ADC_RESOLUTION_BITS < 1 || MIC_ADC_PCM_ADC_RESOLUTION_BITS > 16
#error "MIC_ADC_PCM_ADC_RESOLUTION_BITS must be in range [1, 16]"
#endif

#if MIC_ADC_PCM_DC_TRACK_SHIFT < 1 || MIC_ADC_PCM_DC_TRACK_SHIFT > 15
#error "MIC_ADC_PCM_DC_TRACK_SHIFT must be in range [1, 15]"
#endif

#if MIC_ADC_PCM_GAIN < 1
#error "MIC_ADC_PCM_GAIN must be greater than 0"
#endif

/**
 * @brief ADC 到 PCM 转换器状态。
 *
 * 调用方法：上层先定义一个 mic_adc_pcm_converter_t 变量，再调用
 * mic_adc_pcm_converter_init() 初始化；之后每来一个 ADC raw 样本，就调用
 * mic_adc_pcm_convert_sample() 得到一个 int16_t PCM 样本。
 */
typedef struct {
    int64_t dc_offset_q16; // ADC 直流偏置，Q16 定点格式，用于保留慢速跟踪的小数部分。
    bool dc_offset_ready;  // true 表示 dc_offset_q16 已经完成初始化。
} mic_adc_pcm_converter_t;

/**
 * @brief 初始化 ADC 到 PCM 转换器。
 *
 * 调用方法：
 * @code
 * mic_adc_pcm_converter_t converter;
 * mic_adc_pcm_converter_init(&converter);
 * @endcode
 *
 * @param converter 要初始化的转换器状态，不能为空。
 */
void mic_adc_pcm_converter_init(mic_adc_pcm_converter_t *converter);

/**
 * @brief 手动设置 ADC 直流偏置。
 *
 * 调用方法：安静环境测出 ADC 中心点后调用，用它覆盖默认中心点。
 *
 * @param converter 已初始化或即将使用的转换器状态，不能为空。
 * @param dc_offset_raw ADC raw 中心点，超过当前 ADC 位宽范围时会自动裁剪。
 */
void mic_adc_pcm_converter_set_dc_offset(mic_adc_pcm_converter_t *converter, uint32_t dc_offset_raw);

/**
 * @brief 将单个 ADC raw 样本转换为 int16_t PCM 样本。
 *
 * 调用方法：ADC continuous 解析出一个有效 raw 样本后调用一次。
 *
 * @param converter 转换器状态；传 NULL 时使用固定默认偏置做一次性转换，不做直流跟踪。
 * @param adc_raw ADC 原始采样值，超过当前 ADC 位宽范围时会自动裁剪。
 * @return int16_t PCM 样本，单声道、16000 Hz、little-endian 字节流中的数值本体。
 */
int16_t mic_adc_pcm_convert_sample(mic_adc_pcm_converter_t *converter, uint32_t adc_raw);

/**
 * @brief 批量转换 ADC raw 缓冲为 int16_t PCM 缓冲。
 *
 * 调用方法：当上层已经整理出连续 raw 样本数组时调用，可一次转换多个采样点。
 *
 * @param converter 转换器状态，可为 NULL。
 * @param adc_raw_samples 输入 ADC raw 数组，不能为空。
 * @param pcm_samples 输出 PCM 数组，不能为空，长度至少为 sample_count。
 * @param sample_count 要转换的样本数。
 * @return 实际写入 pcm_samples 的样本数；参数无效时返回 0。
 */
size_t mic_adc_pcm_convert_buffer(mic_adc_pcm_converter_t *converter,
                                  const uint32_t *adc_raw_samples,
                                  int16_t *pcm_samples,
                                  size_t sample_count);

/**
 * @brief 将一个 int16_t PCM 样本打包成 little-endian 两字节。
 *
 * 调用方法：需要把 PCM 写入文件、串口二进制流或网络包时调用。
 *
 * @param pcm_sample 要打包的 PCM 样本。
 * @param out_bytes 输出 2 字节数组；out_bytes[0] 为低字节，out_bytes[1] 为高字节。
 */
void mic_adc_pcm_sample_to_le_bytes(int16_t pcm_sample, uint8_t out_bytes[MIC_ADC_PCM_BYTES_PER_SAMPLE]);

/**
 * @brief 批量把 int16_t PCM 样本打包成 little-endian 字节流。
 *
 * 调用方法：需要生成原始 PCM 文件内容时调用，输出字节数为 sample_count * 2。
 *
 * @param pcm_samples 输入 PCM 样本数组，不能为空。
 * @param out_bytes 输出字节数组，长度至少为 sample_count * MIC_ADC_PCM_BYTES_PER_SAMPLE。
 * @param sample_count 要打包的 PCM 样本数。
 * @return 实际写入 out_bytes 的字节数；参数无效时返回 0。
 */
size_t mic_adc_pcm_samples_to_le_bytes(const int16_t *pcm_samples,
                                       uint8_t *out_bytes,
                                       size_t sample_count);

#endif // MIC_ADC_PCM_H
