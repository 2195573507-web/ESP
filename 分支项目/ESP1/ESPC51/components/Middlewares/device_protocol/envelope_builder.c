/**
 * @file envelope_builder.c
 * @brief C5 feature v2 envelope 序列化器。
 *
 * 本文件只做无状态 JSON 序列化和本地 trace_id 生成。trace_id 用于 S3/Server
 * 日志串联，不代表全局强随机 UUID，也不改变 C5 feature 算法输出。
 */

#include "envelope_builder.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "app_main_config.h"

static uint32_t s_trace_seq;

static bool envelope_text_is_safe(const char *value)
{
    return value != NULL && value[0] != '\0' &&
           strchr(value, '"') == NULL &&
           strchr(value, '\\') == NULL;
}

static bool envelope_local_id_is_valid(const char *value, unsigned int *out)
{
    if (value == NULL || out == NULL) {
        return false;
    }
    if (strcmp(value, "1") == 0) {
        *out = 1U;
        return true;
    }
    if (strcmp(value, "2") == 0) {
        *out = 2U;
        return true;
    }
    return false;
}

static uint32_t hash_bytes(uint32_t hash, const void *data, size_t len)
{
    const uint8_t *bytes = (const uint8_t *)data;
    for (size_t i = 0; i < len; ++i) {
        hash ^= bytes[i];
        hash *= 16777619UL;
    }
    return hash;
}

static uint32_t hash_text(uint32_t hash, const char *text)
{
    return hash_bytes(hash, text, strlen(text));
}

static uint32_t hash_float(uint32_t hash, float value)
{
    union {
        float f;
        uint32_t u;
    } bits = {.f = value};
    return hash_bytes(hash, &bits.u, sizeof(bits.u));
}

static uint32_t next_trace_seq(void)
{
    s_trace_seq++;
    if (s_trace_seq == 0U) {
        s_trace_seq = 1U;
    }
    return s_trace_seq;
}

static void build_trace_id(const envelope_builder_input_t *input,
                           char trace_id[ENVELOPE_BUILDER_TRACE_ID_LEN])
{
    uint32_t seq = next_trace_seq();
    uint32_t hash_a = 2166136261UL;
    uint32_t hash_b = 2166136261UL ^ 0x9e3779b9UL;

    hash_a = hash_text(hash_a, input->device_id);
    hash_a = hash_text(hash_a, input->link_id);
    hash_a = hash_bytes(hash_a, &input->timestamp_ms, sizeof(input->timestamp_ms));
    hash_a = hash_bytes(hash_a, &seq, sizeof(seq));
    hash_a = hash_float(hash_a, input->metrics.frame_energy);
    hash_a = hash_float(hash_a, input->metrics.variance);

    hash_b = hash_text(hash_b, input->source);
    hash_b = hash_float(hash_b, input->metrics.cv);
    hash_b = hash_float(hash_b, input->metrics.quality);
    hash_b = hash_bytes(hash_b, &input->metrics.rssi, sizeof(input->metrics.rssi));
    hash_b = hash_bytes(hash_b, &input->state_hint, sizeof(input->state_hint));
    hash_b = hash_bytes(hash_b, &seq, sizeof(seq));

    /* 生成 UUID 形状的稳定诊断 id；足够用于日志关联，不用于安全鉴权。 */
    uint16_t version_group = (uint16_t)(0x4000U | (hash_b & 0x0fffU));
    uint16_t variant_group = (uint16_t)(0x8000U | ((hash_a >> 8) & 0x3fffU));
    unsigned long long tail =
        ((((unsigned long long)hash_a) << 16) ^ (unsigned long long)hash_b ^ seq) &
        0xffffffffffffULL;

    snprintf(trace_id,
             ENVELOPE_BUILDER_TRACE_ID_LEN,
             "%08lx-%04lx-%04x-%04x-%012llx",
             (unsigned long)hash_a,
             (unsigned long)(hash_b & 0xffffU),
             (unsigned int)version_group,
             (unsigned int)variant_group,
             tail);
}

const char *envelope_builder_state_hint_to_string(envelope_state_hint_t state_hint)
{
    switch (state_hint) {
    case ENVELOPE_STATE_HINT_MOTION:
        return "MOTION";
    case ENVELOPE_STATE_HINT_HOLD:
        return "HOLD";
    case ENVELOPE_STATE_HINT_IDLE:
    default:
        return "IDLE";
    }
}

esp_err_t envelope_builder_format(const envelope_builder_input_t *input,
                                  char *out,
                                  size_t out_size)
{
    if (input == NULL || out == NULL || out_size == 0U ||
        !envelope_text_is_safe(input->device_id) ||
        !envelope_text_is_safe(input->link_id) ||
        !envelope_text_is_safe(input->source) ||
        !isfinite(input->metrics.frame_energy) ||
        !isfinite(input->metrics.variance) ||
        !isfinite(input->metrics.cv) ||
        !isfinite(input->metrics.quality)) {
        return ESP_ERR_INVALID_ARG;
    }

    char trace_id[ENVELOPE_BUILDER_TRACE_ID_LEN] = {0};
    build_trace_id(input, trace_id);

    int written = snprintf(out,
                           out_size,
                           "{\"schema_version\":\"%s\",\"device_id\":\"%s\","
                           "\"link_id\":\"%s\",\"trace_id\":\"%s\","
                           "\"timestamp_ms\":%lld,\"metrics\":{"
                           "\"frame_energy\":%.6g,\"variance\":%.6g,"
                           "\"cv\":%.6g,\"rssi\":%d,\"quality\":%.6g},"
                           "\"state_hint\":\"%s\",\"source\":\"%s\"}",
                           ENVELOPE_BUILDER_SCHEMA_VERSION,
                           input->device_id,
                           input->link_id,
                           trace_id,
                           (long long)input->timestamp_ms,
                           (double)input->metrics.frame_energy,
                           (double)input->metrics.variance,
                           (double)input->metrics.cv,
                           input->metrics.rssi,
                           (double)input->metrics.quality,
                           envelope_builder_state_hint_to_string(input->state_hint),
                           input->source);
    return written > 0 && written < (int)out_size ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

esp_err_t envelope_builder_format_local_csi_report(const envelope_builder_input_t *input,
                                                   char *out,
                                                   size_t out_size)
{
    unsigned int local_id = 0U;
    if (input == NULL || out == NULL || out_size == 0U ||
        !envelope_local_id_is_valid(input->local_id, &local_id) ||
        !envelope_text_is_safe(input->device_id) ||
        !envelope_text_is_safe(input->link_id) ||
        !isfinite(input->metrics.frame_energy) ||
        !isfinite(input->metrics.variance) ||
        !isfinite(input->metrics.cv) ||
        !isfinite(input->metrics.quality) ||
        !isfinite(input->motion_score) ||
        !isfinite(input->confidence) ||
        input->motion_score < 0.0f || input->motion_score > 1.0f ||
        input->confidence < 0.0f || input->confidence > 1.0f) {
        return ESP_ERR_INVALID_ARG;
    }

#if CSI_OUTPUT_ENABLE_DEBUG_METRICS
    int written = snprintf(out,
                           out_size,
                           "{\"id\":%u,\"device_id\":\"%s\",\"lid\":\"%s\",\"t\":%lld,"
                           "\"state\":\"%s\",\"motion_score\":%.6g,\"confidence\":%.6g,"
                           "\"quality\":%.6g,\"rssi\":%d,"
                           "\"energy\":%.6g,\"variance\":%.6g,\"cv\":%.6g,"
                           "\"v\":[%.6g,%.6g,%.6g,%d,%.6g]}",
                           local_id,
                           input->device_id,
                           input->link_id,
                           (long long)input->timestamp_ms,
                           envelope_builder_state_hint_to_string(input->state_hint),
                           (double)input->motion_score,
                           (double)input->confidence,
                           (double)input->metrics.quality,
                           input->metrics.rssi,
                           (double)input->metrics.frame_energy,
                           (double)input->metrics.variance,
                           (double)input->metrics.cv,
                           (double)input->metrics.frame_energy,
                           (double)input->metrics.variance,
                           (double)input->metrics.cv,
                           input->metrics.rssi,
                           (double)input->metrics.quality);
#else
    int written = snprintf(out,
                           out_size,
                           "{\"id\":%u,\"device_id\":\"%s\",\"lid\":\"%s\",\"t\":%lld,"
                           "\"state\":\"%s\",\"motion_score\":%.6g,\"confidence\":%.6g,"
                           "\"quality\":%.6g,\"rssi\":%d,"
                           "\"energy\":%.6g,\"variance\":%.6g,\"cv\":%.6g}",
                           local_id,
                           input->device_id,
                           input->link_id,
                           (long long)input->timestamp_ms,
                           envelope_builder_state_hint_to_string(input->state_hint),
                           (double)input->motion_score,
                           (double)input->confidence,
                           (double)input->metrics.quality,
                           input->metrics.rssi,
                           (double)input->metrics.frame_energy,
                           (double)input->metrics.variance,
                           (double)input->metrics.cv);
#endif
    return written > 0 && written < (int)out_size ? ESP_OK : ESP_ERR_INVALID_SIZE;
}
