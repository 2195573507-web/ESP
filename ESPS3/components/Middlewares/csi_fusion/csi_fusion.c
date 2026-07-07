/**
 * @file csi_fusion.c
 * @brief ESPS3 CanonicalEvent v2 CSI fusion.
 */

#include "csi_fusion.h"

#include <stdio.h>
#include <string.h>

#include "esp_timer.h"
#include "esp111_protocol_common.h"

#define CSI_FUSION_STALE_TICKS 5ULL
#define CSI_FUSION_T_HIGH 0.62f
#define CSI_FUSION_T_LOW 0.30f
#define CSI_FUSION_CONFIRM_TICKS 2U
#define CSI_FUSION_HOLD_TICKS 3U
#define CSI_FUSION_MIN_WEIGHT 0.01f

typedef struct {
    bool valid;
    csi_fusion_feature_t feature;
} csi_fusion_tick_sample_t;

typedef struct {
    bool configured;
    char name[CSI_FUSION_TEXT_LEN];
    char primary_link_id[CSI_FUSION_TEXT_LEN];
    csi_fusion_tick_sample_t pending_sample;
} csi_fusion_link_t;

static csi_fusion_link_t s_links[CSI_FUSION_LINK_COUNT];
static csi_fusion_state_t s_state;
static uint8_t s_motion_candidate_ticks;
static uint8_t s_idle_candidate_ticks;
static uint64_t s_current_tick_id;
static uint64_t s_last_finalized_tick_id;
static bool s_has_current_tick;
static bool s_has_finalized_tick;

static float clamp01(float value)
{
    if (value < 0.0f) {
        return 0.0f;
    }
    if (value > 1.0f) {
        return 1.0f;
    }
    return value;
}

static uint64_t now_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000);
}

static uint64_t tick_id_for_timestamp(uint64_t timestamp_ms)
{
    return timestamp_ms / (uint64_t)CSI_FUSION_TICK_MS;
}

static uint64_t timestamp_for_tick(uint64_t tick_id)
{
    return tick_id * (uint64_t)CSI_FUSION_TICK_MS;
}

static uint64_t feature_timestamp_ms(const csi_fusion_feature_t *feature)
{
    if (feature != NULL && feature->timestamp_ms > 0ULL) {
        return feature->timestamp_ms;
    }
    return now_ms();
}

static void reset_state_to_idle(void)
{
    s_state = CSI_FUSION_STATE_IDLE;
    s_motion_candidate_ticks = 0U;
    s_idle_candidate_ticks = 0U;
}

static void reset_link(csi_fusion_link_t *link,
                       const char *name,
                       const char *primary_link_id)
{
    if (link == NULL) {
        return;
    }
    memset(link, 0, sizeof(*link));
    link->configured = true;
    strlcpy(link->name, name, sizeof(link->name));
    strlcpy(link->primary_link_id, primary_link_id, sizeof(link->primary_link_id));
}

void csi_fusion_init(void)
{
    reset_link(&s_links[0], "C51", "S3_TO_C51");
    reset_link(&s_links[1], "C52", "S3_TO_C52");
    reset_state_to_idle();
    s_current_tick_id = 0ULL;
    s_last_finalized_tick_id = 0ULL;
    s_has_current_tick = false;
    s_has_finalized_tick = false;
}

const char *csi_fusion_link_state_name(size_t index)
{
    if (index >= CSI_FUSION_LINK_COUNT || !s_links[index].configured) {
        return "";
    }
    return s_links[index].name;
}

static int link_index_for_feature(const csi_fusion_feature_t *feature)
{
    if (feature == NULL) {
        return -1;
    }
    if (strcmp(feature->link_id, "S3_TO_C51") == 0 ||
        strcmp(feature->link_id, "C52_TO_C51") == 0 ||
        strcmp(feature->link_id, "C51") == 0 ||
        strcmp(feature->device_id, "C51") == 0 ||
        strcmp(feature->device_id, ESP111_PROTOCOL_TERMINAL_DEVICE_ID_C51) == 0) {
        return 0;
    }
    if (strcmp(feature->link_id, "S3_TO_C52") == 0 ||
        strcmp(feature->link_id, "C51_TO_C52") == 0 ||
        strcmp(feature->link_id, "C52") == 0 ||
        strcmp(feature->device_id, "C52") == 0 ||
        strcmp(feature->device_id, ESP111_PROTOCOL_TERMINAL_DEVICE_ID_C52) == 0) {
        return 1;
    }
    return -1;
}

static bool feature_valid_for_fusion(const csi_fusion_feature_t *feature)
{
    return feature != NULL &&
           feature->link_id[0] != '\0' &&
           feature->confidence >= 0.0f &&
           feature->confidence <= 1.0f &&
           feature->quality >= 0.0f &&
           feature->quality <= 1.0f;
}

static float freshness_for_ticks(uint64_t sample_tick, uint64_t tick_id)
{
    if (sample_tick > tick_id) {
        return 1.0f;
    }
    uint64_t age_ticks = tick_id - sample_tick;
    if (age_ticks >= CSI_FUSION_STALE_TICKS) {
        return 0.0f;
    }
    float freshness = 1.0f - ((float)age_ticks / (float)CSI_FUSION_STALE_TICKS);
    return freshness > CSI_FUSION_MIN_WEIGHT ? freshness : CSI_FUSION_MIN_WEIGHT;
}

static float link_weight(const csi_fusion_link_state_t *state, uint64_t tick_id)
{
    if (state == NULL || !state->valid) {
        return 0.0f;
    }
    float quality = clamp01(state->quality);
    if (quality <= 0.0f) {
        return 0.0f;
    }
    float freshness = freshness_for_ticks(state->tick_id, tick_id);
    return quality * freshness;
}

static csi_fusion_link_state_t link_state_from_feature(const csi_fusion_feature_t *feature,
                                                       const csi_fusion_link_t *link,
                                                       uint64_t tick_id)
{
    csi_fusion_link_state_t out = {0};
    if (feature == NULL || link == NULL) {
        return out;
    }

    out.valid = true;
    strlcpy(out.device_id, feature->device_id, sizeof(out.device_id));
    strlcpy(out.link_id, link->primary_link_id, sizeof(out.link_id));
    strlcpy(out.trace_id, feature->trace_id, sizeof(out.trace_id));
    out.has_state = feature->has_state;
    out.state = feature->state;
    out.confidence = clamp01(feature->confidence);
    out.quality = clamp01(feature->quality);
    out.rssi = feature->rssi;
    out.frame_seq = feature->frame_seq;
    out.tick_id = tick_id;
    out.timestamp_ms = timestamp_for_tick(tick_id);
    return out;
}

static void update_state_machine(float confidence, uint8_t active_link_count)
{
    if (active_link_count == 0U) {
        confidence = 0.0f;
    }

    bool motion_evidence = confidence >= CSI_FUSION_T_HIGH;
    bool idle_evidence = confidence <= CSI_FUSION_T_LOW;

    if (s_state == CSI_FUSION_STATE_IDLE) {
        s_idle_candidate_ticks = 0U;
        if (!motion_evidence) {
            s_motion_candidate_ticks = 0U;
            return;
        }
        if (s_motion_candidate_ticks < UINT8_MAX) {
            ++s_motion_candidate_ticks;
        }
        if (s_motion_candidate_ticks >= CSI_FUSION_CONFIRM_TICKS) {
            s_state = CSI_FUSION_STATE_MOTION;
            s_motion_candidate_ticks = 0U;
        }
        return;
    }

    if (motion_evidence) {
        s_state = CSI_FUSION_STATE_MOTION;
        s_motion_candidate_ticks = 0U;
        s_idle_candidate_ticks = 0U;
        return;
    }

    s_motion_candidate_ticks = 0U;
    if (idle_evidence) {
        if (s_idle_candidate_ticks < UINT8_MAX) {
            ++s_idle_candidate_ticks;
        }
        if (s_idle_candidate_ticks >= CSI_FUSION_HOLD_TICKS) {
            s_state = CSI_FUSION_STATE_IDLE;
            s_idle_candidate_ticks = 0U;
        } else {
            s_state = CSI_FUSION_STATE_HOLD;
        }
    } else {
        s_state = CSI_FUSION_STATE_HOLD;
        s_idle_candidate_ticks = 0U;
    }
}

static bool tick_has_all_links(uint64_t tick_id)
{
    for (size_t i = 0; i < CSI_FUSION_LINK_COUNT; ++i) {
        if (!s_links[i].pending_sample.valid ||
            tick_id_for_timestamp(feature_timestamp_ms(&s_links[i].pending_sample.feature)) != tick_id) {
            return false;
        }
    }
    return true;
}

static void clear_tick_samples(uint64_t tick_id)
{
    for (size_t i = 0; i < CSI_FUSION_LINK_COUNT; ++i) {
        if (s_links[i].pending_sample.valid &&
            tick_id_for_timestamp(feature_timestamp_ms(&s_links[i].pending_sample.feature)) == tick_id) {
            memset(&s_links[i].pending_sample, 0, sizeof(s_links[i].pending_sample));
        }
    }
}

static void trace_id_for_event(const csi_fusion_link_state_t *links,
                               char *out,
                               size_t out_size,
                               uint64_t tick_id)
{
    if (out == NULL || out_size == 0U) {
        return;
    }
    out[0] = '\0';

    for (size_t i = 0; i < CSI_FUSION_LINK_COUNT; ++i) {
        if (links[i].valid && links[i].trace_id[0] != '\0') {
            strlcpy(out, links[i].trace_id, out_size);
            return;
        }
    }

    (void)snprintf(out, out_size, "csi-v2-%llu", (unsigned long long)tick_id);
}

static void copy_event_outputs(const csi_fusion_canonical_event_t *event,
                               csi_fusion_fact_t *out_fact,
                               csi_fusion_telemetry_t *out_telemetry)
{
    if (out_fact != NULL && event != NULL) {
        *out_fact = *event;
    }
    if (out_telemetry != NULL && event != NULL) {
        *out_telemetry = *event;
    }
}

static bool finalize_tick(uint64_t tick_id,
                          csi_fusion_fact_t *out_fact,
                          csi_fusion_telemetry_t *out_telemetry)
{
    if (s_has_finalized_tick && tick_id <= s_last_finalized_tick_id) {
        if (out_fact != NULL) {
            memset(out_fact, 0, sizeof(*out_fact));
        }
        if (out_telemetry != NULL) {
            memset(out_telemetry, 0, sizeof(*out_telemetry));
        }
        return false;
    }

    csi_fusion_link_state_t link_states[CSI_FUSION_LINK_COUNT] = {0};
    float weighted_confidence = 0.0f;
    float weight_total = 0.0f;
    uint8_t active_link_count = 0U;

    for (size_t i = 0; i < CSI_FUSION_LINK_COUNT; ++i) {
        csi_fusion_link_t *link = &s_links[i];
        if (link->pending_sample.valid &&
            tick_id_for_timestamp(feature_timestamp_ms(&link->pending_sample.feature)) == tick_id) {
            link_states[i] = link_state_from_feature(&link->pending_sample.feature, link, tick_id);
        }

        float weight = link_weight(&link_states[i], tick_id);
        if (weight <= 0.0f) {
            continue;
        }
        weighted_confidence += link_states[i].confidence * weight;
        weight_total += weight;
        ++active_link_count;
    }

    float confidence = weight_total > 0.0f ? clamp01(weighted_confidence / weight_total) : 0.0f;
    update_state_machine(confidence, active_link_count);

    clear_tick_samples(tick_id);
    s_last_finalized_tick_id = tick_id;
    s_has_finalized_tick = true;

    if (active_link_count == 0U) {
        return false;
    }

    csi_fusion_canonical_event_t event = {0};
    event.valid = true;
    event.schema_version = CSI_FUSION_SCHEMA_VERSION;
    event.tick_id = tick_id;
    event.fused_state = s_state;
    event.confidence = confidence;
    event.timestamp_ms = timestamp_for_tick(tick_id);
    event.active_link_count = active_link_count;
    trace_id_for_event(link_states, event.trace_id, sizeof(event.trace_id), tick_id);

    uint8_t out_link_index = 0U;
    for (size_t i = 0; i < CSI_FUSION_LINK_COUNT && out_link_index < CSI_FUSION_LINK_COUNT; ++i) {
        if (!link_states[i].valid) {
            continue;
        }
        strlcpy(event.links[out_link_index],
                link_states[i].link_id,
                sizeof(event.links[out_link_index]));
        ++out_link_index;
    }

    copy_event_outputs(&event, out_fact, out_telemetry);
    return true;
}

esp_err_t csi_fusion_update(const csi_fusion_feature_t *feature,
                            csi_fusion_fact_t *out_fact,
                            csi_fusion_telemetry_t *out_telemetry)
{
    if (feature == NULL || out_fact == NULL || !feature_valid_for_fusion(feature)) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out_fact, 0, sizeof(*out_fact));
    if (out_telemetry != NULL) {
        memset(out_telemetry, 0, sizeof(*out_telemetry));
    }

    int link_index = link_index_for_feature(feature);
    if (link_index < 0 || link_index >= (int)CSI_FUSION_LINK_COUNT) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    uint64_t sample_timestamp_ms = feature_timestamp_ms(feature);
    uint64_t tick_id = feature->tick_id > 0ULL ? feature->tick_id : tick_id_for_timestamp(sample_timestamp_ms);
    if (!s_has_current_tick) {
        s_current_tick_id = tick_id;
        s_has_current_tick = true;
    }

    if (s_has_finalized_tick && tick_id <= s_last_finalized_tick_id) {
        return ESP_OK;
    }

    bool emitted = false;
    if (tick_id > s_current_tick_id) {
        emitted = finalize_tick(s_current_tick_id, out_fact, out_telemetry);
        while (s_current_tick_id + 1ULL < tick_id) {
            ++s_current_tick_id;
            (void)finalize_tick(s_current_tick_id, NULL, NULL);
        }
        s_current_tick_id = tick_id;
    } else if (tick_id + CSI_FUSION_STALE_TICKS < s_current_tick_id) {
        return ESP_ERR_INVALID_ARG;
    }

    csi_fusion_link_t *link = &s_links[link_index];
    link->pending_sample.valid = true;
    link->pending_sample.feature = *feature;
    link->pending_sample.feature.timestamp_ms = sample_timestamp_ms;
    link->pending_sample.feature.tick_id = tick_id;

    if (!emitted && tick_has_all_links(tick_id)) {
        (void)finalize_tick(tick_id, out_fact, out_telemetry);
    }

    return ESP_OK;
}

esp_err_t csi_fusion_flush(csi_fusion_fact_t *out_fact,
                           csi_fusion_telemetry_t *out_telemetry)
{
    if (out_fact == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out_fact, 0, sizeof(*out_fact));
    if (out_telemetry != NULL) {
        memset(out_telemetry, 0, sizeof(*out_telemetry));
    }

    if (!s_has_current_tick) {
        return ESP_OK;
    }

    uint64_t now_tick = tick_id_for_timestamp(now_ms());
    if (now_tick <= s_current_tick_id) {
        return ESP_OK;
    }

    (void)finalize_tick(s_current_tick_id, out_fact, out_telemetry);
    while (s_current_tick_id + 1ULL < now_tick) {
        ++s_current_tick_id;
        (void)finalize_tick(s_current_tick_id, NULL, NULL);
    }
    s_current_tick_id = now_tick;
    return ESP_OK;
}

esp_err_t csi_fusion_format_telemetry_json(const csi_fusion_telemetry_t *telemetry,
                                           char *out,
                                           size_t out_size)
{
    if (telemetry == NULL || out == NULL || out_size == 0U || !telemetry->valid) {
        return ESP_ERR_INVALID_ARG;
    }

    char links_json[(CSI_FUSION_TEXT_LEN * CSI_FUSION_LINK_COUNT) + 8U] = "[";
    size_t used = 1U;
    for (uint8_t i = 0; i < telemetry->active_link_count && i < CSI_FUSION_LINK_COUNT; ++i) {
        int written = snprintf(links_json + used,
                               sizeof(links_json) - used,
                               "%s\"%s\"",
                               i == 0U ? "" : ",",
                               telemetry->links[i]);
        if (written <= 0 || (size_t)written >= sizeof(links_json) - used) {
            return ESP_ERR_INVALID_SIZE;
        }
        used += (size_t)written;
    }
    if (used + 1U >= sizeof(links_json)) {
        return ESP_ERR_INVALID_SIZE;
    }
    links_json[used++] = ']';
    links_json[used] = '\0';

    const char *state = csi_fusion_state_to_string(telemetry->fused_state);
    int written = snprintf(out,
                           out_size,
                           "{\"type\":\"csi_fusion\",\"schema_version\":%u,"
                           "\"trace_id\":\"%s\",\"tick_id\":%llu,\"links\":%s,"
                           "\"fused_state\":{\"state\":\"%s\",\"confidence\":%.3f,"
                           "\"motion_score\":%.3f},\"confidence\":%.3f,"
                           "\"motion_score\":%.3f,\"timestamp_ms\":%llu}",
                           (unsigned int)telemetry->schema_version,
                           telemetry->trace_id,
                           (unsigned long long)telemetry->tick_id,
                           links_json,
                           state,
                           (double)telemetry->confidence,
                           (double)telemetry->confidence,
                           (double)telemetry->confidence,
                           (double)telemetry->confidence,
                           (unsigned long long)telemetry->timestamp_ms);
    return written > 0 && written < (int)out_size ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

const char *csi_fusion_state_to_string(csi_fusion_state_t state)
{
    switch (state) {
    case CSI_FUSION_STATE_MOTION:
        return "MOTION";
    case CSI_FUSION_STATE_HOLD:
        return "HOLD";
    case CSI_FUSION_STATE_IDLE:
    default:
        return "IDLE";
    }
}

bool csi_fusion_state_from_string(const char *value, csi_fusion_state_t *out_state)
{
    if (value == NULL || value[0] == '\0' || out_state == NULL) {
        return false;
    }
    if (strcmp(value, "MOTION") == 0 || strcmp(value, "motion") == 0 ||
        strcmp(value, "occupied") == 0) {
        *out_state = CSI_FUSION_STATE_MOTION;
        return true;
    }
    if (strcmp(value, "HOLD") == 0 || strcmp(value, "hold") == 0) {
        *out_state = CSI_FUSION_STATE_HOLD;
        return true;
    }
    if (strcmp(value, "IDLE") == 0 || strcmp(value, "idle") == 0 ||
        strcmp(value, "vacant") == 0) {
        *out_state = CSI_FUSION_STATE_IDLE;
        return true;
    }
    return false;
}
