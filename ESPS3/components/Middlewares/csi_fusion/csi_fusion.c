/**
 * @file csi_fusion.c
 * @brief ESPS3 CSI feature fusion and state-machine implementation.
 */

#include "csi_fusion.h"

#include <math.h>
#include <string.h>

#include "gateway_config.h"

#define CSI_FUSION_MAX_LINKS 4U
#define CSI_FUSION_WINDOW 16U
#define CSI_FUSION_STALE_MS 5000U
#define CSI_FUSION_ALPHA 0.25f
#define CSI_FUSION_W_ENERGY 1.0f
#define CSI_FUSION_W_DELTA 1.2f
#define CSI_FUSION_SCORE_BUFFER_ALPHA 0.35f
#define CSI_FUSION_SCORE_DECAY 0.82f
#define CSI_FUSION_T_HIGH 0.58f
#define CSI_FUSION_T_LOW 0.28f
#define CSI_FUSION_CONFIRM_FRAMES 2U
#define CSI_FUSION_HOLD_FRAMES 3U
#define CSI_FUSION_WINDOW_TIME_MS 1000U
#define CSI_FUSION_RSSI_MIN -90
#define CSI_FUSION_RSSI_MAX -35

typedef struct {
    uint16_t count;
    float mean;
    float m2;
} csi_fusion_stat_t;

typedef struct {
    bool valid;
    char device_id[CSI_FUSION_TEXT_LEN];
    char link_id[CSI_FUSION_TEXT_LEN];
    csi_fusion_stat_t energy_stats;
    csi_fusion_stat_t variance_stats;
    csi_fusion_stat_t delta_stats;
    float energy_ewma;
    float energy_window[CSI_FUSION_WINDOW];
    uint8_t count;
    uint8_t next_index;
    bool has_previous_energy;
    float previous_energy;
    float latest_frame_energy;
    float latest_variance;
    float latest_quality;
    int latest_rssi;
    uint32_t latest_frame_seq;
    uint64_t latest_timestamp_ms;
    float latest_score;
    float latest_stability;
} csi_fusion_link_t;

static csi_fusion_link_t s_links[CSI_FUSION_MAX_LINKS];
static csi_fusion_state_t s_state;
static uint8_t s_motion_candidate_count;
static uint8_t s_idle_candidate_count;
static uint64_t s_motion_candidate_since_ms;
static float s_score_buffer;
static uint64_t s_last_fact_timestamp_ms;

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

static float absf_local(float value)
{
    return value < 0.0f ? -value : value;
}

static float sigmoidf_local(float value)
{
    if (value > 10.0f) {
        return 1.0f;
    }
    if (value < -10.0f) {
        return 0.0f;
    }
    return 1.0f / (1.0f + expf(-value));
}

static void stat_update(csi_fusion_stat_t *stat, float value)
{
    if (stat == NULL) {
        return;
    }

    if (stat->count < UINT16_MAX) {
        stat->count++;
    }
    float delta = value - stat->mean;
    stat->mean += delta / (float)stat->count;
    float delta2 = value - stat->mean;
    stat->m2 += delta * delta2;
}

static float stat_stddev(const csi_fusion_stat_t *stat)
{
    if (stat == NULL || stat->count < 2U) {
        return 0.0f;
    }
    float variance = stat->m2 / (float)(stat->count - 1U);
    return variance > 0.0001f ? sqrtf(variance) : 0.0f;
}

static float stat_zscore(const csi_fusion_stat_t *stat, float value)
{
    if (stat == NULL || stat->count < 4U) {
        return 0.0f;
    }
    float stddev = stat_stddev(stat);
    if (stddev <= 0.0001f) {
        return 0.0f;
    }
    float z = (value - stat->mean) / stddev;
    if (z > 6.0f) {
        return 6.0f;
    }
    if (z < -6.0f) {
        return -6.0f;
    }
    return z;
}

static csi_fusion_link_t *find_or_alloc_link(const char *link_id)
{
    if (link_id == NULL || link_id[0] == '\0') {
        return NULL;
    }

    for (size_t i = 0; i < CSI_FUSION_MAX_LINKS; ++i) {
        if (s_links[i].valid && strcmp(s_links[i].link_id, link_id) == 0) {
            return &s_links[i];
        }
    }
    for (size_t i = 0; i < CSI_FUSION_MAX_LINKS; ++i) {
        if (!s_links[i].valid) {
            memset(&s_links[i], 0, sizeof(s_links[i]));
            s_links[i].valid = true;
            strlcpy(s_links[i].link_id, link_id, sizeof(s_links[i].link_id));
            return &s_links[i];
        }
    }
    return &s_links[0];
}

static float window_variance(const csi_fusion_link_t *link)
{
    if (link == NULL || link->count == 0U) {
        return 0.0f;
    }

    float sum = 0.0f;
    for (uint8_t i = 0; i < link->count; ++i) {
        sum += link->energy_window[i];
    }
    float mean = sum / (float)link->count;
    float squared = 0.0f;
    for (uint8_t i = 0; i < link->count; ++i) {
        float diff = link->energy_window[i] - mean;
        squared += diff * diff;
    }
    return squared / (float)link->count;
}

static float score_link(csi_fusion_link_t *link, const csi_fusion_feature_t *feature)
{
    if (link == NULL || feature == NULL) {
        return 0.0f;
    }

    float energy_delta = link->has_previous_energy
                             ? absf_local(feature->frame_energy - link->previous_energy)
                             : 0.0f;
    float z_energy = stat_zscore(&link->energy_stats, feature->frame_energy);
    float z_variance = stat_zscore(&link->variance_stats, feature->variance);
    float z_delta = stat_zscore(&link->delta_stats, energy_delta);

    if (link->count == 0U) {
        link->energy_ewma = feature->frame_energy;
    } else {
        link->energy_ewma = (CSI_FUSION_ALPHA * feature->frame_energy) +
                            ((1.0f - CSI_FUSION_ALPHA) * link->energy_ewma);
    }

    link->energy_window[link->next_index] = feature->frame_energy;
    link->next_index = (uint8_t)((link->next_index + 1U) % CSI_FUSION_WINDOW);
    if (link->count < CSI_FUSION_WINDOW) {
        ++link->count;
    }

    stat_update(&link->energy_stats, feature->frame_energy);
    stat_update(&link->variance_stats, feature->variance);
    stat_update(&link->delta_stats, energy_delta);
    link->previous_energy = feature->frame_energy;
    link->has_previous_energy = true;

    float rolling_variance = window_variance(link);
    float variance_stability = 1.0f / (1.0f + absf_local(z_variance) + rolling_variance);
    float delta_stability = 1.0f / (1.0f + (0.25f * absf_local(z_delta)));
    link->latest_stability = clamp01(variance_stability * delta_stability);

    return clamp01(sigmoidf_local((CSI_FUSION_W_ENERGY * z_energy) +
                                  (CSI_FUSION_W_DELTA * z_delta)));
}

static bool link_is_fresh(const csi_fusion_link_t *link, uint64_t now_ms)
{
    if (link == NULL || !link->valid || link->latest_timestamp_ms == 0U) {
        return false;
    }
    if (now_ms < link->latest_timestamp_ms) {
        return true;
    }
    return now_ms - link->latest_timestamp_ms <= CSI_FUSION_STALE_MS;
}

static float link_freshness(const csi_fusion_link_t *link, uint64_t now_ms)
{
    if (link == NULL || !link_is_fresh(link, now_ms)) {
        return 0.0f;
    }
    if (now_ms <= link->latest_timestamp_ms) {
        return 1.0f;
    }
    uint64_t age_ms = now_ms - link->latest_timestamp_ms;
    if (age_ms >= CSI_FUSION_STALE_MS) {
        return 0.0f;
    }
    return clamp01(1.0f - ((float)age_ms / (float)CSI_FUSION_STALE_MS));
}

static float rssi_weight(int rssi)
{
    if (rssi <= CSI_FUSION_RSSI_MIN) {
        return 0.05f;
    }
    if (rssi >= CSI_FUSION_RSSI_MAX) {
        return 1.0f;
    }
    return ((float)(rssi - CSI_FUSION_RSSI_MIN)) /
           ((float)(CSI_FUSION_RSSI_MAX - CSI_FUSION_RSSI_MIN));
}

static float link_weight(const csi_fusion_link_t *link, uint64_t now_ms)
{
    if (link == NULL) {
        return 0.0f;
    }
    float base_weight = rssi_weight(link->latest_rssi) *
                        clamp01(link->latest_stability) *
                        link_freshness(link, now_ms);
    return base_weight * clamp01(link->latest_quality);
}

static void reset_state_to_idle(void)
{
    s_state = CSI_FUSION_STATE_IDLE;
    s_motion_candidate_count = 0U;
    s_idle_candidate_count = 0U;
    s_motion_candidate_since_ms = 0U;
    s_score_buffer = 0.0f;
}

static void update_state_machine(float fused_score, uint64_t timestamp_ms)
{
    float input = fused_score;
    if (fused_score <= 0.5f) {
        input *= 0.5f;
    }
    if (fused_score >= s_score_buffer) {
        s_score_buffer = (CSI_FUSION_SCORE_BUFFER_ALPHA * fused_score) +
                         ((1.0f - CSI_FUSION_SCORE_BUFFER_ALPHA) * s_score_buffer);
    } else {
        float decayed = s_score_buffer * CSI_FUSION_SCORE_DECAY;
        s_score_buffer = (CSI_FUSION_SCORE_BUFFER_ALPHA * input) +
                         ((1.0f - CSI_FUSION_SCORE_BUFFER_ALPHA) * decayed);
    }
    s_score_buffer = clamp01(s_score_buffer);

    bool motion_evidence = fused_score >= CSI_FUSION_T_HIGH ||
                           s_score_buffer >= CSI_FUSION_T_HIGH;
    bool idle_evidence = fused_score <= CSI_FUSION_T_LOW ||
                         s_score_buffer <= CSI_FUSION_T_LOW;

    if (s_state == CSI_FUSION_STATE_IDLE) {
        s_idle_candidate_count = 0U;
        if (!motion_evidence) {
            s_motion_candidate_count = 0U;
            s_motion_candidate_since_ms = 0U;
            return;
        }

        if (s_motion_candidate_count == 0U) {
            s_motion_candidate_since_ms = timestamp_ms;
        }
        if (s_motion_candidate_count < UINT8_MAX) {
            s_motion_candidate_count++;
        }
        uint64_t elapsed = timestamp_ms >= s_motion_candidate_since_ms
                               ? timestamp_ms - s_motion_candidate_since_ms
                               : 0U;
        if (s_motion_candidate_count >= CSI_FUSION_CONFIRM_FRAMES &&
            elapsed >= ((uint64_t)CSI_FUSION_CONFIRM_FRAMES * CSI_FUSION_WINDOW_TIME_MS)) {
            s_state = CSI_FUSION_STATE_MOTION;
            s_motion_candidate_count = 0U;
            s_motion_candidate_since_ms = 0U;
        }
        return;
    }

    if (motion_evidence) {
        s_state = CSI_FUSION_STATE_MOTION;
        s_idle_candidate_count = 0U;
        s_motion_candidate_count = 0U;
        s_motion_candidate_since_ms = 0U;
        return;
    }

    s_motion_candidate_count = 0U;
    s_motion_candidate_since_ms = 0U;
    if (idle_evidence) {
        if (s_idle_candidate_count < UINT8_MAX) {
            s_idle_candidate_count++;
        }
        if (s_idle_candidate_count >= CSI_FUSION_HOLD_FRAMES) {
            s_state = CSI_FUSION_STATE_IDLE;
            s_idle_candidate_count = 0U;
        } else {
            s_state = CSI_FUSION_STATE_HOLD;
        }
    } else {
        s_state = CSI_FUSION_STATE_HOLD;
        s_idle_candidate_count = 0U;
    }
}

void csi_fusion_init(void)
{
    memset(s_links, 0, sizeof(s_links));
    reset_state_to_idle();
    s_last_fact_timestamp_ms = 0U;
}

esp_err_t csi_fusion_update(const csi_fusion_feature_t *feature,
                            csi_fusion_fact_t *out_fact)
{
    if (feature == NULL || out_fact == NULL || feature->link_id[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    csi_fusion_link_t *link = find_or_alloc_link(feature->link_id);
    if (link == NULL) {
        return ESP_ERR_NO_MEM;
    }

    strlcpy(link->device_id, feature->device_id, sizeof(link->device_id));
    link->latest_frame_energy = feature->frame_energy;
    link->latest_variance = feature->variance;
    link->latest_quality = feature->quality;
    link->latest_rssi = feature->rssi;
    link->latest_frame_seq = feature->frame_seq;
    link->latest_timestamp_ms = feature->timestamp_ms;
    link->latest_score = score_link(link, feature);

    float weighted_sum = 0.0f;
    float weight_total = 0.0f;
    float energy_sum = 0.0f;
    float variance_sum = 0.0f;
    float rssi_sum = 0.0f;
    uint8_t active_count = 0U;

    for (size_t i = 0; i < CSI_FUSION_MAX_LINKS; ++i) {
        if (!link_is_fresh(&s_links[i], feature->timestamp_ms)) {
            continue;
        }
        float weight = link_weight(&s_links[i], feature->timestamp_ms);
        if (weight <= 0.0f) {
            continue;
        }
        weighted_sum += s_links[i].latest_score * weight;
        weight_total += weight;
        energy_sum += s_links[i].latest_frame_energy * weight;
        variance_sum += s_links[i].latest_variance * weight;
        rssi_sum += (float)s_links[i].latest_rssi * weight;
        ++active_count;
    }

    float fused_score = weight_total > 0.0f ? clamp01(weighted_sum / weight_total) : 0.0f;
    if (active_count == 0U) {
        reset_state_to_idle();
    } else {
        update_state_machine(fused_score, feature->timestamp_ms);
    }

    memset(out_fact, 0, sizeof(*out_fact));
    out_fact->valid = active_count > 0U;
    strlcpy(out_fact->device_id, gateway_config_get()->gateway_id, sizeof(out_fact->device_id));
    strlcpy(out_fact->link_id, "fused", sizeof(out_fact->link_id));
    out_fact->state = s_state;
    out_fact->motion_score = active_count > 0U ? fused_score : 0.0f;
    out_fact->frame_energy = weight_total > 0.0f ? energy_sum / weight_total : 0.0f;
    out_fact->variance = weight_total > 0.0f ? variance_sum / weight_total : 0.0f;
    out_fact->rssi = weight_total > 0.0f ? (int)(rssi_sum / weight_total) : 0;
    out_fact->timestamp_ms = feature->timestamp_ms;
    out_fact->active_link_count = active_count;
    s_last_fact_timestamp_ms = feature->timestamp_ms;
    return ESP_OK;
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
