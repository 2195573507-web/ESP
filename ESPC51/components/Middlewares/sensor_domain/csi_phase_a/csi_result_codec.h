#ifndef CSI_RESULT_CODEC_H
#define CSI_RESULT_CODEC_H

/**
 * @file csi_result_codec.h
 * @brief 阶段 A CSI 结果摘要编码接口。
 *
 * 本文件只把 csi_presence_result 转成轻量结构体或日志字符串。阶段 A 不发 HTTP、
 * 不访问 /local/v1/csi/result、不上传 S3/Server，也不编码 raw CSI。
 */

#include "csi_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t schema_version;
    uint8_t state_code;
    uint8_t motion_score_pct;
    float mean_amplitude;
    float variance;
    float cv;
    uint8_t quality_code;
    int8_t rssi;
    uint16_t sample_count;
    uint64_t updated_at_ms;
} csi_result_codec_summary_t;

bool csi_result_codec_from_presence(const csi_presence_result_t *result,
                                    csi_result_codec_summary_t *out_summary);

int csi_result_codec_format_summary(const csi_result_codec_summary_t *summary,
                                    char *buffer,
                                    size_t buffer_size);

#ifdef __cplusplus
}
#endif

#endif /* CSI_RESULT_CODEC_H */
