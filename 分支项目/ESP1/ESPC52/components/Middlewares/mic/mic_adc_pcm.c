#include "mic_adc_pcm.h"

#include <limits.h>

/**
 * @brief PCM 转换内部固定常量。
 *
 * 调用方法：仅供本文件内部计算使用，不属于后期调试参数，因此不放在头文件暴露。
 * - MIC_ADC_PCM_Q16_ONE：Q16 定点缩放系数，ADC raw 左移 16 bit 后保存直流偏置小数部分。
 */
enum {
    MIC_ADC_PCM_Q16_ONE = 65536,
};

/**
 * @brief 裁剪 ADC raw 到当前配置位宽允许的范围。
 *
 * 调用方法：所有单样本转换入口先调用本函数，保证后续缩放不会使用异常 raw 值。
 *
 * @param adc_raw 输入 ADC 原始值。
 * @return 裁剪后的 ADC 原始值。
 */
static uint32_t mic_adc_pcm_clip_adc_raw(uint32_t adc_raw)
{
    return adc_raw > MIC_ADC_PCM_ADC_RAW_MAX ? MIC_ADC_PCM_ADC_RAW_MAX : adc_raw;
}

/**
 * @brief 将 32 位中间结果饱和裁剪成 int16_t。
 *
 * 调用方法：ADC 去直流、缩放、增益处理后调用，防止声音过大时整数溢出。
 *
 * @param value 需要裁剪的有符号中间值。
 * @return 裁剪后的 int16_t PCM 样本。
 */
static int16_t mic_adc_pcm_saturate_int16(int32_t value)
{
    if (value > INT16_MAX) {
        return INT16_MAX;
    }
    if (value < INT16_MIN) {
        return INT16_MIN;
    }
    return (int16_t)value;
}

/**
 * @brief 把去直流后的 ADC raw 差值乘以 PCM 增益。
 *
 * 调用方法：mic_adc_pcm_convert_sample() 得到 centered_raw 后调用。
 *
 * @param centered_raw 已减去直流偏置的 ADC raw 差值。
 * @return 已乘增益、但尚未裁剪到 int16_t 的中间值。
 */
static int32_t mic_adc_pcm_scale_centered_raw(int32_t centered_raw)
{
    int64_t scaled = (int64_t)centered_raw * MIC_ADC_PCM_GAIN;
    if (scaled > INT32_MAX) {
        return INT32_MAX;
    }
    if (scaled < INT32_MIN) {
        return INT32_MIN;
    }
    return (int32_t)scaled;
}

/**
 * @brief 根据最新 raw 样本缓慢更新直流偏置。
 *
 * 调用方法：mic_adc_pcm_convert_sample() 完成当前样本转换后调用，让偏置跟随慢变化。
 *
 * @param converter 转换器状态，不能为空。
 * @param raw_q16 当前 ADC raw 的 Q16 表示。
 */
static void mic_adc_pcm_track_dc_offset(mic_adc_pcm_converter_t *converter, int64_t raw_q16)
{
#if MIC_ADC_PCM_ENABLE_DC_TRACK
    int64_t delta = raw_q16 - converter->dc_offset_q16;
    converter->dc_offset_q16 += delta / (1 << MIC_ADC_PCM_DC_TRACK_SHIFT);
#else
    (void)converter;
    (void)raw_q16;
#endif
}

/**
 * @brief 初始化 ADC 到 PCM 转换器状态。
 *
 * 调用方法：开始转换一段新的 ADC 音频流前调用一次；之后持续复用同一个 converter，
 * 让直流偏置跟踪能够连续工作。
 *
 * @param converter 要初始化的转换器状态，不能为空；传 NULL 时函数直接返回。
 */
void mic_adc_pcm_converter_init(mic_adc_pcm_converter_t *converter)
{
    if (converter == NULL) {
        return;
    }

    converter->dc_offset_q16 = (int64_t)mic_adc_pcm_clip_adc_raw(MIC_ADC_PCM_DEFAULT_DC_OFFSET_RAW) *
                               MIC_ADC_PCM_Q16_ONE;
    converter->dc_offset_ready = MIC_ADC_PCM_AUTO_SEED_DC_OFFSET ? false : true;
}

/**
 * @brief 手动覆盖 ADC 直流偏置中心点。
 *
 * 调用方法：如果安静环境下已经测得稳定 raw 平均值，可在初始化后调用本函数，
 * 让后续 PCM 以该 raw 平均值为 0 点。
 *
 * @param converter 要设置的转换器状态，不能为空；传 NULL 时函数直接返回。
 * @param dc_offset_raw ADC raw 中心点，超出位宽范围会被自动裁剪。
 */
void mic_adc_pcm_converter_set_dc_offset(mic_adc_pcm_converter_t *converter, uint32_t dc_offset_raw)
{
    if (converter == NULL) {
        return;
    }

    converter->dc_offset_q16 = (int64_t)mic_adc_pcm_clip_adc_raw(dc_offset_raw) * MIC_ADC_PCM_Q16_ONE;
    converter->dc_offset_ready = true;
}

/**
 * @brief 将一个 ADC raw 样本转换为 int16_t PCM 样本。
 *
 * 调用方法：ADC continuous 每解析到一个有效 Mic raw 样本，就调用一次本函数；
 * 返回值可直接作为 16000 Hz、单声道、int16_t PCM 的一个采样点。
 *
 * @param converter 转换器状态；传 NULL 时使用默认直流偏置做无状态转换。
 * @param adc_raw ADC 原始采样值，超出位宽范围会被自动裁剪。
 * @return 转换后的 int16_t PCM 样本。
 */
int16_t mic_adc_pcm_convert_sample(mic_adc_pcm_converter_t *converter, uint32_t adc_raw)
{
    uint32_t clipped_raw = mic_adc_pcm_clip_adc_raw(adc_raw);
    int64_t raw_q16 = (int64_t)clipped_raw * MIC_ADC_PCM_Q16_ONE;
    int64_t dc_offset_q16 = (int64_t)mic_adc_pcm_clip_adc_raw(MIC_ADC_PCM_DEFAULT_DC_OFFSET_RAW) *
                            MIC_ADC_PCM_Q16_ONE;

    if (converter != NULL) {
        if (!converter->dc_offset_ready) {
            converter->dc_offset_q16 = raw_q16;
            converter->dc_offset_ready = true;
            return 0;
        }
        dc_offset_q16 = converter->dc_offset_q16;
    }

    int32_t centered_raw = (int32_t)((raw_q16 - dc_offset_q16) / MIC_ADC_PCM_Q16_ONE);
    int16_t pcm_sample = mic_adc_pcm_saturate_int16(mic_adc_pcm_scale_centered_raw(centered_raw));

    if (converter != NULL) {
        mic_adc_pcm_track_dc_offset(converter, raw_q16);
    }

    return pcm_sample;
}

/**
 * @brief 批量转换 ADC raw 数组为 int16_t PCM 数组。
 *
 * 调用方法：如果上层已经把一段 ADC raw 整理成数组，可调用本函数一次性转换；
 * converter 会按数组顺序连续更新直流偏置。
 *
 * @param converter 转换器状态；传 NULL 时每个样本都使用默认直流偏置做无状态转换。
 * @param adc_raw_samples 输入 ADC raw 数组，不能为空。
 * @param pcm_samples 输出 PCM 数组，不能为空。
 * @param sample_count 要转换的样本数。
 * @return 实际写入的 PCM 样本数；参数无效时返回 0。
 */
size_t mic_adc_pcm_convert_buffer(mic_adc_pcm_converter_t *converter,
                                  const uint32_t *adc_raw_samples,
                                  int16_t *pcm_samples,
                                  size_t sample_count)
{
    if (adc_raw_samples == NULL || pcm_samples == NULL) {
        return 0;
    }

    for (size_t i = 0; i < sample_count; i++) {
        pcm_samples[i] = mic_adc_pcm_convert_sample(converter, adc_raw_samples[i]);
    }
    return sample_count;
}

/**
 * @brief 将一个 int16_t PCM 样本打包成 little-endian 字节。
 *
 * 调用方法：写 PCM 文件、发送二进制音频帧或对接网络协议前调用；
 * 输出固定为低字节在前、高字节在后。
 *
 * @param pcm_sample 要打包的 PCM 样本。
 * @param out_bytes 输出 2 字节缓存；传 NULL 时函数直接返回。
 */
void mic_adc_pcm_sample_to_le_bytes(int16_t pcm_sample, uint8_t out_bytes[MIC_ADC_PCM_BYTES_PER_SAMPLE])
{
    if (out_bytes == NULL) {
        return;
    }

    uint16_t packed = (uint16_t)pcm_sample;
    out_bytes[0] = (uint8_t)(packed & 0xFFU);
    out_bytes[1] = (uint8_t)((packed >> 8) & 0xFFU);
}

/**
 * @brief 批量把 int16_t PCM 数组打包成 little-endian 字节流。
 *
 * 调用方法：需要生成裸 PCM 数据块时调用，输出长度固定为 sample_count * 2 字节。
 *
 * @param pcm_samples 输入 PCM 数组，不能为空。
 * @param out_bytes 输出字节缓存，长度至少为 sample_count * MIC_ADC_PCM_BYTES_PER_SAMPLE。
 * @param sample_count 要打包的 PCM 样本数。
 * @return 实际写入的字节数；参数无效时返回 0。
 */
size_t mic_adc_pcm_samples_to_le_bytes(const int16_t *pcm_samples,
                                       uint8_t *out_bytes,
                                       size_t sample_count)
{
    if (pcm_samples == NULL || out_bytes == NULL) {
        return 0;
    }

    for (size_t i = 0; i < sample_count; i++) {
        mic_adc_pcm_sample_to_le_bytes(pcm_samples[i],
                                       &out_bytes[i * MIC_ADC_PCM_BYTES_PER_SAMPLE]);
    }
    return sample_count * MIC_ADC_PCM_BYTES_PER_SAMPLE;
}
