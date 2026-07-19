/**
 * @file csi_fusion.c
 * @brief ESPS3 CanonicalEvent v2 CSI fusion.
 */

#include "csi_fusion.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp111_protocol_common.h"

#define CSI_FUSION_SAMPLE_FRESH_MS 3000ULL
#define CSI_FUSION_EMPTY_LOG_MS 1000ULL
#define CSI_FUSION_RESTORE_WARMUP_TICKS 5U
#define CSI_FUSION_T_HIGH 0.62f
#define CSI_FUSION_T_LOW 0.30f
#define CSI_FUSION_CONFIRM_TICKS 5U
#define CSI_FUSION_HOLD_TICKS 20U
#define CSI_FUSION_RSSI_STRONG_DBM (-45)
#define CSI_FUSION_RSSI_WEAK_DBM (-90)
#define CSI_FUSION_RSSI_UNKNOWN_WEIGHT 0.75f
#define CSI_FUSION_VARIANCE_IDLE 80.0f
#define CSI_FUSION_VARIANCE_MOTION 260.0f
#define CSI_FUSION_CV_IDLE 0.08f
#define CSI_FUSION_CV_MOTION 0.22f
#define CSI_FUSION_ENERGY_REFERENCE 24.0f
#define CSI_FUSION_MOTION_SCORE_WEIGHT 0.70f
#define CSI_FUSION_METRICS_SCORE_WEIGHT 0.30f

static const char *TAG = "csi_fusion";

typedef struct {
    bool valid;
    csi_fusion_feature_t feature;
} csi_fusion_tick_sample_t;

typedef struct {
    bool configured;
    bool active;
    uint8_t warmup_tick_count;
    uint64_t last_warmup_tick_id;
    char name[CSI_FUSION_TEXT_LEN];
    char primary_link_id[CSI_FUSION_TEXT_LEN];
    csi_fusion_tick_sample_t latest_sample;
} csi_fusion_link_t;

static csi_fusion_link_t s_links[CSI_FUSION_LINK_COUNT];
static csi_fusion_state_t s_state;
static uint8_t s_motion_candidate_ticks;
static uint8_t s_idle_candidate_ticks;
static uint64_t s_current_tick_id;
static uint64_t s_last_finalized_tick_id;
static uint64_t s_last_empty_log_ms;
static bool s_has_current_tick;
static bool s_has_finalized_tick;

static bool sample_age_ms(const csi_fusion_tick_sample_t *sample,
                          uint64_t reference_ms,
                          uint64_t *out_age_ms);

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

static float freshness_weight_for_sample(const csi_fusion_tick_sample_t *sample,
                                         uint64_t reference_ms)
{
    uint64_t age_ms = 0ULL;
    if (!sample_age_ms(sample, reference_ms, &age_ms) ||
        age_ms >= CSI_FUSION_SAMPLE_FRESH_MS) {
        return 0.0f;
    }
    return 1.0f - ((float)age_ms / (float)CSI_FUSION_SAMPLE_FRESH_MS);
}

static float rssi_weight(int rssi)
{
    if (rssi == 0) {
        return CSI_FUSION_RSSI_UNKNOWN_WEIGHT;
    }
    if (rssi >= CSI_FUSION_RSSI_STRONG_DBM) {
        return 1.0f;
    }
    if (rssi <= CSI_FUSION_RSSI_WEAK_DBM) {
        return 0.15f;
    }

    float span = (float)(CSI_FUSION_RSSI_STRONG_DBM - CSI_FUSION_RSSI_WEAK_DBM);
    float normalized = (float)(rssi - CSI_FUSION_RSSI_WEAK_DBM) / span;
    return 0.15f + (0.85f * clamp01(normalized));
}

static float normalize_metric_range(float value, float low, float high)
{
    if (!isfinite(value) || high <= low) {
        return 0.0f;
    }
    return clamp01((value - low) / (high - low));
}

static float normalize_energy(float energy)
{
    if (!isfinite(energy) || energy <= 0.0f) {
        return 0.0f;
    }
    return clamp01(energy / (energy + CSI_FUSION_ENERGY_REFERENCE));
}

static float metrics_motion_score(const csi_fusion_link_state_t *state)
{
    if (state == NULL || !state->valid || !state->has_metrics) {
        return state != NULL && state->valid ? clamp01(state->motion_score) : 0.0f;
    }

    float metric_score =
        (0.20f * normalize_energy(state->energy)) +
        (0.45f * normalize_metric_range(state->variance,
                                        CSI_FUSION_VARIANCE_IDLE,
                                        CSI_FUSION_VARIANCE_MOTION)) +
        (0.35f * normalize_metric_range(state->cv,
                                        CSI_FUSION_CV_IDLE,
                                        CSI_FUSION_CV_MOTION));
    return clamp01((CSI_FUSION_MOTION_SCORE_WEIGHT * clamp01(state->motion_score)) +
                   (CSI_FUSION_METRICS_SCORE_WEIGHT * metric_score));
}

static float base_weight_for_fusion(const csi_fusion_link_state_t *state,
                                    const csi_fusion_tick_sample_t *sample,
                                    uint64_t reference_ms)
{
    if (state == NULL || !state->valid) {
        return 0.0f;
    }
    return clamp01(state->quality) *
           freshness_weight_for_sample(sample, reference_ms) *
           rssi_weight(state->rssi);
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

static uint64_t sample_timestamp_ms(const csi_fusion_tick_sample_t *sample)
{
    if (sample == NULL || !sample->valid || sample->feature.timestamp_ms == 0ULL) {
        return 0ULL;
    }
    return sample->feature.timestamp_ms;
}

static bool sample_age_ms(const csi_fusion_tick_sample_t *sample,
                          uint64_t reference_ms,
                          uint64_t *out_age_ms)
{
    uint64_t sample_ms = sample_timestamp_ms(sample);
    if (sample_ms == 0ULL || out_age_ms == NULL) {
        return false;
    }
    *out_age_ms = sample_ms > reference_ms ? 0ULL : reference_ms - sample_ms;
    return true;
}

static int64_t sample_age_log_ms(const csi_fusion_tick_sample_t *sample,
                                 uint64_t reference_ms)
{
    uint64_t age_ms = 0ULL;
    if (!sample_age_ms(sample, reference_ms, &age_ms)) {
        return -1;
    }
    return age_ms > (uint64_t)INT64_MAX ? INT64_MAX : (int64_t)age_ms;
}

static bool sample_is_fresh(const csi_fusion_tick_sample_t *sample,
                            uint64_t reference_ms,
                            uint64_t *out_age_ms)
{
    uint64_t age_ms = 0ULL;
    if (!sample_age_ms(sample, reference_ms, &age_ms)) {
        return false;
    }
    if (out_age_ms != NULL) {
        *out_age_ms = age_ms;
    }
    return age_ms <= CSI_FUSION_SAMPLE_FRESH_MS;
}

static uint64_t feature_tick_id(const csi_fusion_feature_t *feature)
{
    if (feature != NULL && feature->tick_id > 0ULL) {
        return feature->tick_id;
    }
    return tick_id_for_timestamp(now_ms());
}

static void reset_state_to_idle(void)
{
    s_state = CSI_FUSION_STATE_IDLE;
    s_motion_candidate_ticks = 0U;
    s_idle_candidate_ticks = 0U;
}

static bool any_link_active(void)
{
    for (size_t i = 0; i < CSI_FUSION_LINK_COUNT; ++i) {
        if (s_links[i].configured && s_links[i].active) {
            return true;
        }
    }
    return false;
}

static void reset_global_runtime(void)
{
    reset_state_to_idle();
    s_current_tick_id = 0ULL;
    s_last_finalized_tick_id = 0ULL;
    s_last_empty_log_ms = 0ULL;
    s_has_current_tick = false;
    s_has_finalized_tick = false;
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
    link->active = false;
    strlcpy(link->name, name, sizeof(link->name));
    strlcpy(link->primary_link_id, primary_link_id, sizeof(link->primary_link_id));
}

void csi_fusion_init(void)
{
    reset_link(&s_links[0], "C51", "S3_TO_C51");
    reset_link(&s_links[1], "C52", "S3_TO_C52");
    reset_global_runtime();
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
    if (strcmp(feature->link_id, "S3_TO_C51") == 0) {
        return 0;
    }
    if (strcmp(feature->link_id, "S3_TO_C52") == 0) {
        return 1;
    }
    return -1;
}

static int link_index_for_device_id(const char *device_id)
{
    if (device_id == NULL || device_id[0] == '\0') {
        return -1;
    }
    if (strcmp(device_id, "C51") == 0 ||
        strcmp(device_id, ESP111_PROTOCOL_TERMINAL_DEVICE_ID_C51) == 0) {
        return 0;
    }
    if (strcmp(device_id, "C52") == 0 ||
        strcmp(device_id, ESP111_PROTOCOL_TERMINAL_DEVICE_ID_C52) == 0) {
        return 1;
    }
    return -1;
}

esp_err_t csi_fusion_suspend_link(const char *device_id)
{
    int link_index = link_index_for_device_id(device_id);
    if (link_index < 0 || link_index >= (int)CSI_FUSION_LINK_COUNT) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    csi_fusion_link_t *link = &s_links[link_index];
    const bool was_active = link->active;
    link->active = false;
    link->warmup_tick_count = 0U;
    link->last_warmup_tick_id = 0ULL;
    memset(&link->latest_sample, 0, sizeof(link->latest_sample));

    if (was_active) {
        /* A topology change invalidates fused hysteresis even if another link remains active. */
        reset_global_runtime();
    }
    return ESP_OK;
}

esp_err_t csi_fusion_restore_link(const char *device_id)
{
    int link_index = link_index_for_device_id(device_id);
    if (link_index < 0 || link_index >= (int)CSI_FUSION_LINK_COUNT) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    csi_fusion_link_t *link = &s_links[link_index];
    if (link->active) {
        return ESP_OK;
    }

    /* Rejoining links start from a fresh fused tick/state boundary. */
    reset_global_runtime();
    memset(&link->latest_sample, 0, sizeof(link->latest_sample));
    link->warmup_tick_count = 0U;
    link->last_warmup_tick_id = 0ULL;
    link->active = true;
    return ESP_OK;
}

bool csi_fusion_link_warmup_complete(const char *device_id)
{
    int link_index = link_index_for_device_id(device_id);
    if (link_index < 0 || link_index >= (int)CSI_FUSION_LINK_COUNT) {
        return false;
    }

    const csi_fusion_link_t *link = &s_links[link_index];
    return link->active &&
           link->warmup_tick_count >= CSI_FUSION_RESTORE_WARMUP_TICKS &&
           link->latest_sample.valid;
}

static bool feature_valid_for_fusion(const csi_fusion_feature_t *feature)
{
    return feature != NULL &&
           feature->link_id[0] != '\0' &&
           feature->motion_score >= 0.0f &&
           feature->motion_score <= 1.0f &&
           feature->confidence >= 0.0f &&
           feature->confidence <= 1.0f &&
           feature->quality >= 0.0f &&
           feature->quality <= 1.0f;
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
    out.motion_score = clamp01(feature->motion_score);
    out.confidence = clamp01(feature->confidence);
    out.quality = clamp01(feature->quality);
    out.rssi = feature->rssi;
    out.has_metrics = feature->has_metrics;
    out.energy = feature->energy;
    out.variance = feature->variance;
    out.cv = feature->cv;
    out.frame_seq = feature->frame_seq;
    out.tick_id = tick_id;
    out.timestamp_ms = feature->timestamp_ms > 0ULL ? feature->timestamp_ms :
                                                       timestamp_for_tick(tick_id);
    return out;
}

static void update_state_machine(float motion_score, uint8_t active_link_count)
{
    if (active_link_count == 0U) {
        return;
    }

    bool motion_evidence = motion_score >= CSI_FUSION_T_HIGH;
    bool idle_evidence = motion_score <= CSI_FUSION_T_LOW;

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

static void log_empty_fusion_if_due(uint64_t reference_ms)
{
    if (!any_link_active()) {
        return;
    }
    if (s_last_empty_log_ms != 0ULL &&
        reference_ms - s_last_empty_log_ms < CSI_FUSION_EMPTY_LOG_MS) {
        return;
    }
    s_last_empty_log_ms = reference_ms;
    ESP_LOGW(TAG,
             "CSI_FUSION_EMPTY reason=no_fresh_sample now_ms=%llu c51_age_ms=%lld c52_age_ms=%lld fresh_ms=%llu",
             (unsigned long long)reference_ms,
             (long long)sample_age_log_ms(&s_links[0].latest_sample, reference_ms),
             (long long)sample_age_log_ms(&s_links[1].latest_sample, reference_ms),
             (unsigned long long)CSI_FUSION_SAMPLE_FRESH_MS);
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
    float weighted_motion_score = 0.0f;
    float weight_total = 0.0f;
    uint8_t active_link_count = 0U;
    uint64_t reference_ms = now_ms();
    uint64_t tick_ms = timestamp_for_tick(tick_id);
    if (tick_ms > reference_ms) {
        reference_ms = tick_ms;
    }

    for (size_t i = 0; i < CSI_FUSION_LINK_COUNT; ++i) {
        csi_fusion_link_t *link = &s_links[i];
        if (!link->active) {
            continue;
        }
        if (sample_is_fresh(&link->latest_sample, reference_ms, NULL)) {
            link_states[i] = link_state_from_feature(&link->latest_sample.feature, link, tick_id);
        }

        float weight = base_weight_for_fusion(&link_states[i],
                                              &link->latest_sample,
                                              reference_ms);
        if (weight <= 0.0f) {
            continue;
        }
        weighted_motion_score += metrics_motion_score(&link_states[i]) * weight;
        weight_total += weight;
        ++active_link_count;
    }

    float motion_score = weight_total > 0.0f ? clamp01(weighted_motion_score / weight_total) : 0.0f;
    float confidence = active_link_count > 0U ? clamp01(weight_total / (float)active_link_count) : 0.0f;
    update_state_machine(motion_score, active_link_count);

    s_last_finalized_tick_id = tick_id;
    s_has_finalized_tick = true;

    if (active_link_count == 0U) {
        log_empty_fusion_if_due(reference_ms);
        csi_fusion_canonical_event_t telemetry = {0};
        telemetry.valid = true;
        telemetry.schema_version = CSI_FUSION_SCHEMA_VERSION;
        telemetry.tick_id = tick_id;
        telemetry.fused_state = CSI_FUSION_STATE_IDLE;
        telemetry.motion_score = 0.0f;
        telemetry.confidence = 0.0f;
        telemetry.timestamp_ms = timestamp_for_tick(tick_id);
        telemetry.active_link_count = 0U;
        trace_id_for_event(link_states, telemetry.trace_id, sizeof(telemetry.trace_id), tick_id);
        if (out_fact != NULL) {
            memset(out_fact, 0, sizeof(*out_fact));
        }
        if (out_telemetry != NULL) {
            *out_telemetry = telemetry;
        }
        return false;
    }

    csi_fusion_canonical_event_t event = {0};
    event.valid = true;
    event.schema_version = CSI_FUSION_SCHEMA_VERSION;
    event.tick_id = tick_id;
    event.fused_state = s_state;
    event.motion_score = motion_score;
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

    csi_fusion_link_t *link = &s_links[link_index];
    if (!link->active) {
        return ESP_ERR_INVALID_STATE;
    }

    int device_link_index = link_index_for_device_id(feature->device_id);
    if (device_link_index >= 0 && device_link_index != link_index) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    uint64_t tick_id = feature_tick_id(feature);
    uint64_t sample_timestamp_ms = feature->timestamp_ms > 0ULL ?
                                       feature->timestamp_ms :
                                       timestamp_for_tick(tick_id);
    if (!s_has_current_tick) {
        s_current_tick_id = tick_id;
        s_has_current_tick = true;
    }

    if (s_has_finalized_tick && tick_id <= s_last_finalized_tick_id) {
        return ESP_OK;
    }

    if (tick_id > s_current_tick_id) {
        (void)finalize_tick(s_current_tick_id, out_fact, out_telemetry);
        while (s_current_tick_id + 1ULL < tick_id) {
            ++s_current_tick_id;
            (void)finalize_tick(s_current_tick_id, NULL, NULL);
        }
        s_current_tick_id = tick_id;
    } else if (tick_id < s_current_tick_id) {
        return ESP_OK;
    }

    if (link->warmup_tick_count < CSI_FUSION_RESTORE_WARMUP_TICKS) {
        if (link->warmup_tick_count == 0U || tick_id != link->last_warmup_tick_id) {
            link->last_warmup_tick_id = tick_id;
            ++link->warmup_tick_count;
        }
        if (link->warmup_tick_count < CSI_FUSION_RESTORE_WARMUP_TICKS) {
            memset(&link->latest_sample, 0, sizeof(link->latest_sample));
            return ESP_OK;
        }
    }

    link->latest_sample.valid = true;
    link->latest_sample.feature = *feature;
    link->latest_sample.feature.timestamp_ms = sample_timestamp_ms;
    link->latest_sample.feature.tick_id = tick_id;

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

    if (!any_link_active() || !s_has_current_tick) {
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
                           (double)telemetry->motion_score,
                           (double)telemetry->confidence,
                           (double)telemetry->motion_score,
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
