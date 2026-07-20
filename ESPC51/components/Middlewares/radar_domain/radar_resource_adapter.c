#include "radar_resource_adapter.h"

#include <limits.h>
#include <string.h>

#include "app_runtime.h"
#include "c5_resource_manager.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

#define C5_RADAR_PRESENT_CONFIRM_FRAMES 2U
#define C5_RADAR_MOVING_CONFIRM_FRAMES 2U
#define C5_RADAR_MOVING_EXIT_MS 1500U
#define C5_RADAR_RECENT_VACANT_MS 10000U
#define C5_RADAR_STALE_MS 1000U
#define C5_RADAR_VOICE_RECOVERY_MS 2000U
#define C5_RADAR_MOVING_SPEED_CM_S 16

#define C5_RADAR_VACANT_STABLE_UPLOAD_MS 5000U
#define C5_RADAR_VACANT_RECENT_UPLOAD_MS 1000U
#define C5_RADAR_PRESENT_STILL_UPLOAD_MS 500U
#define C5_RADAR_PRESENT_MOVING_UPLOAD_MS 100U
#define C5_RADAR_DEGRADED_UPLOAD_MS 5000U
#define C5_RADAR_VOICE_UPLOAD_MS 1000U

#define C5_BME_VACANT_STABLE_UPLOAD_MS 10000U
#define C5_BME_VACANT_RECENT_UPLOAD_MS 20000U
#define C5_BME_PRESENT_STILL_UPLOAD_MS 30000U
#define C5_BME_PRESENT_MOVING_UPLOAD_MS 60000U
#define C5_BME_DEGRADED_UPLOAD_MS 20000U
#define C5_BME_VOICE_EVENT_MS 1000U

typedef struct {
    bool initialized;
    bool link_online;
    bool has_latest;
    bool upload_inflight;
    bool voice_active;
    bool force_radar_upload;
    bool force_bme_upload;
    uint8_t link_state;
    uint32_t latest_generation;
    uint32_t inflight_generation;
    uint32_t request_sequence;
    uint32_t retry_delay_ms;
    uint32_t present_streak;
    uint32_t moving_streak;
    uint64_t last_valid_ms;
    uint64_t last_motion_ms;
    uint64_t no_target_since_ms;
    uint64_t last_radar_upload_ms;
    uint64_t last_bme_upload_ms;
    uint64_t next_radar_attempt_ms;
    uint64_t voice_recovery_until_ms;
    c5_sense_mode_t mode;
    radar_target_sample_t latest;
    radar_resource_adapter_stats_t stats;
} radar_resource_adapter_context_t;

static radar_resource_adapter_context_t s_adapter;
static portMUX_TYPE s_adapter_lock = portMUX_INITIALIZER_UNLOCKED;

static void saturating_increment(uint32_t *value)
{
    if (value != NULL && *value < UINT32_MAX) {
        ++(*value);
    }
}

static bool elapsed(uint64_t now_ms, uint64_t then_ms, uint32_t period_ms)
{
    return then_ms == 0U || now_ms < then_ms || now_ms - then_ms >= period_ms;
}

static bool sample_has_movement(const radar_target_sample_t *sample)
{
    if (sample == NULL || !sample->sample_valid) {
        return false;
    }
    for (uint8_t i = 0U; i < sample->target_count; ++i) {
        const int16_t speed = sample->targets[i].speed_cm_s;
        const int32_t absolute_speed = speed < 0 ? -(int32_t)speed : speed;
        if (absolute_speed >= C5_RADAR_MOVING_SPEED_CM_S) {
            return true;
        }
    }
    return false;
}

static bool voice_busy(void)
{
#if CONFIG_C5_VOICE_TELEMETRY_THROTTLE
    return c5_resource_manager_is_voice_exclusive() || app_runtime_non_voice_is_paused();
#else
    return false;
#endif
}

const char *radar_resource_adapter_mode_name(c5_sense_mode_t mode)
{
    switch (mode) {
    case C5_SENSE_VACANT_STABLE: return "VACANT_STABLE";
    case C5_SENSE_VACANT_RECENT: return "VACANT_RECENT";
    case C5_SENSE_PRESENT_STILL: return "PRESENT_STILL";
    case C5_SENSE_PRESENT_MOVING: return "PRESENT_MOVING";
    case C5_SENSE_RADAR_DEGRADED: return "RADAR_DEGRADED";
    case C5_SENSE_VOICE_ACTIVE: return "VOICE_ACTIVE";
    default: return "UNKNOWN";
    }
}

static uint32_t radar_upload_period(c5_sense_mode_t mode)
{
    switch (mode) {
    case C5_SENSE_PRESENT_MOVING: return C5_RADAR_PRESENT_MOVING_UPLOAD_MS;
    case C5_SENSE_PRESENT_STILL: return C5_RADAR_PRESENT_STILL_UPLOAD_MS;
    case C5_SENSE_VACANT_RECENT: return C5_RADAR_VACANT_RECENT_UPLOAD_MS;
    case C5_SENSE_RADAR_DEGRADED: return C5_RADAR_DEGRADED_UPLOAD_MS;
    case C5_SENSE_VOICE_ACTIVE: return C5_RADAR_VOICE_UPLOAD_MS;
    case C5_SENSE_VACANT_STABLE:
    default: return C5_RADAR_VACANT_STABLE_UPLOAD_MS;
    }
}

static uint32_t bme_upload_period(c5_sense_mode_t mode)
{
    switch (mode) {
    case C5_SENSE_PRESENT_MOVING: return C5_BME_PRESENT_MOVING_UPLOAD_MS;
    case C5_SENSE_PRESENT_STILL: return C5_BME_PRESENT_STILL_UPLOAD_MS;
    case C5_SENSE_VACANT_RECENT: return C5_BME_VACANT_RECENT_UPLOAD_MS;
    case C5_SENSE_RADAR_DEGRADED: return C5_BME_DEGRADED_UPLOAD_MS;
    case C5_SENSE_VOICE_ACTIVE: return UINT32_MAX;
    case C5_SENSE_VACANT_STABLE:
    default: return C5_BME_VACANT_STABLE_UPLOAD_MS;
    }
}

static void update_mode_locked(uint64_t now_ms)
{
    const bool now_voice_active = voice_busy();
    if (s_adapter.voice_active && !now_voice_active) {
        s_adapter.voice_recovery_until_ms = now_ms + C5_RADAR_VOICE_RECOVERY_MS;
        s_adapter.force_radar_upload = true;
        s_adapter.force_bme_upload = true;
    }
    s_adapter.voice_active = now_voice_active;

    c5_sense_mode_t next_mode;
    if (now_voice_active || (s_adapter.voice_recovery_until_ms != 0U &&
                             now_ms < s_adapter.voice_recovery_until_ms)) {
        next_mode = C5_SENSE_VOICE_ACTIVE;
    } else if (!s_adapter.link_online || s_adapter.last_valid_ms == 0U ||
               elapsed(now_ms, s_adapter.last_valid_ms, C5_RADAR_STALE_MS)) {
        next_mode = C5_SENSE_RADAR_DEGRADED;
    } else if (s_adapter.moving_streak >= C5_RADAR_MOVING_CONFIRM_FRAMES ||
               (s_adapter.last_motion_ms != 0U && !elapsed(now_ms, s_adapter.last_motion_ms,
                                                           C5_RADAR_MOVING_EXIT_MS))) {
        next_mode = C5_SENSE_PRESENT_MOVING;
    } else if (s_adapter.present_streak >= C5_RADAR_PRESENT_CONFIRM_FRAMES) {
        next_mode = C5_SENSE_PRESENT_STILL;
    } else if (s_adapter.no_target_since_ms == 0U ||
               !elapsed(now_ms, s_adapter.no_target_since_ms, C5_RADAR_RECENT_VACANT_MS)) {
        next_mode = C5_SENSE_VACANT_RECENT;
    } else {
        next_mode = C5_SENSE_VACANT_STABLE;
    }

    if (next_mode != s_adapter.mode) {
        s_adapter.mode = next_mode;
        s_adapter.force_radar_upload = true;
        saturating_increment(&s_adapter.stats.radar_state_transition_upload_count);
    }
    s_adapter.stats.mode = s_adapter.mode;
    s_adapter.stats.voice_active = s_adapter.voice_active;
    s_adapter.stats.latest_pending = s_adapter.has_latest &&
        s_adapter.latest_generation != s_adapter.inflight_generation;
}

void radar_resource_adapter_init(uint64_t now_ms)
{
    portENTER_CRITICAL(&s_adapter_lock);
    memset(&s_adapter, 0, sizeof(s_adapter));
    s_adapter.initialized = true;
    s_adapter.request_sequence = 1U;
    s_adapter.no_target_since_ms = now_ms;
    s_adapter.mode = C5_SENSE_RADAR_DEGRADED;
    s_adapter.stats.mode = s_adapter.mode;
    portEXIT_CRITICAL(&s_adapter_lock);
}

void radar_resource_adapter_deinit(void)
{
    portENTER_CRITICAL(&s_adapter_lock);
    memset(&s_adapter, 0, sizeof(s_adapter));
    portEXIT_CRITICAL(&s_adapter_lock);
}

void radar_resource_adapter_update_sample(const radar_target_sample_t *sample,
                                          uint64_t now_ms)
{
    if (sample == NULL) {
        return;
    }
    portENTER_CRITICAL(&s_adapter_lock);
    if (!s_adapter.initialized) {
        portEXIT_CRITICAL(&s_adapter_lock);
        radar_resource_adapter_init(now_ms);
        portENTER_CRITICAL(&s_adapter_lock);
    }
    if (s_adapter.has_latest && s_adapter.latest_generation != s_adapter.inflight_generation) {
        saturating_increment(&s_adapter.stats.radar_upload_coalesce_count);
    }
    s_adapter.latest = *sample;
    s_adapter.link_state = sample->link_state;
    s_adapter.link_online = sample->link_state == 5U;
    s_adapter.has_latest = true;
    ++s_adapter.latest_generation;
    if (s_adapter.latest_generation == 0U) {
        s_adapter.latest_generation = 1U;
    }
    saturating_increment(&s_adapter.stats.radar_generated_count);

    if (sample->sample_valid) {
        s_adapter.last_valid_ms = now_ms;
        if (sample->target_count > 0U) {
            if (s_adapter.present_streak < UINT32_MAX) {
                ++s_adapter.present_streak;
            }
            s_adapter.no_target_since_ms = 0U;
            if (sample_has_movement(sample)) {
                if (s_adapter.moving_streak < UINT32_MAX) {
                    ++s_adapter.moving_streak;
                }
                s_adapter.last_motion_ms = now_ms;
            } else {
                s_adapter.moving_streak = 0U;
            }
        } else {
            s_adapter.present_streak = 0U;
            s_adapter.moving_streak = 0U;
            if (s_adapter.no_target_since_ms == 0U) {
                s_adapter.no_target_since_ms = now_ms;
            }
        }
    }
    update_mode_locked(now_ms);
    portEXIT_CRITICAL(&s_adapter_lock);
}

void radar_resource_adapter_set_link_state(uint8_t link_state,
                                           bool online,
                                           uint64_t now_ms)
{
    portENTER_CRITICAL(&s_adapter_lock);
    if (!s_adapter.initialized) {
        portEXIT_CRITICAL(&s_adapter_lock);
        radar_resource_adapter_init(now_ms);
        portENTER_CRITICAL(&s_adapter_lock);
    }
    if (s_adapter.link_online != online || s_adapter.link_state != link_state) {
        s_adapter.link_online = online;
        s_adapter.link_state = link_state;
        s_adapter.force_radar_upload = true;
    }
    update_mode_locked(now_ms);
    portEXIT_CRITICAL(&s_adapter_lock);
}

void radar_resource_adapter_tick(uint64_t now_ms)
{
    portENTER_CRITICAL(&s_adapter_lock);
    if (s_adapter.initialized) {
        update_mode_locked(now_ms);
    }
    portEXIT_CRITICAL(&s_adapter_lock);
}

bool radar_resource_adapter_take_radar_upload(uint64_t now_ms,
                                              radar_target_sample_t *out_sample,
                                              uint32_t *out_sequence)
{
    if (out_sample == NULL || out_sequence == NULL) {
        return false;
    }
    radar_resource_adapter_tick(now_ms);
    portENTER_CRITICAL(&s_adapter_lock);
    const bool cooling_down = s_adapter.voice_recovery_until_ms != 0U &&
                              now_ms < s_adapter.voice_recovery_until_ms;
    if (!s_adapter.has_latest || s_adapter.upload_inflight || s_adapter.voice_active ||
        cooling_down || (s_adapter.next_radar_attempt_ms != 0U &&
                          now_ms < s_adapter.next_radar_attempt_ms)) {
        portEXIT_CRITICAL(&s_adapter_lock);
        return false;
    }
    const bool due = s_adapter.force_radar_upload ||
                     elapsed(now_ms, s_adapter.last_radar_upload_ms,
                             radar_upload_period(s_adapter.mode));
    if (!due) {
        portEXIT_CRITICAL(&s_adapter_lock);
        return false;
    }

    *out_sample = s_adapter.latest;
    if (s_adapter.mode == C5_SENSE_RADAR_DEGRADED) {
        out_sample->sample_valid = false;
        out_sample->target_count = 0U;
        memset(out_sample->targets, 0, sizeof(out_sample->targets));
    }
    out_sample->link_state = s_adapter.link_state;
    *out_sequence = s_adapter.request_sequence++;
    if (s_adapter.request_sequence == 0U) {
        s_adapter.request_sequence = 1U;
    }
    s_adapter.inflight_generation = s_adapter.latest_generation;
    s_adapter.upload_inflight = true;
    s_adapter.force_radar_upload = false;
    saturating_increment(&s_adapter.stats.radar_upload_attempt_count);
    s_adapter.stats.latest_pending = false;
    portEXIT_CRITICAL(&s_adapter_lock);
    return true;
}

void radar_resource_adapter_complete_radar_upload(bool success, uint64_t now_ms)
{
    portENTER_CRITICAL(&s_adapter_lock);
    if (!s_adapter.upload_inflight) {
        portEXIT_CRITICAL(&s_adapter_lock);
        return;
    }
    s_adapter.upload_inflight = false;
    if (success) {
        s_adapter.last_radar_upload_ms = now_ms;
        s_adapter.next_radar_attempt_ms = 0U;
        s_adapter.retry_delay_ms = 0U;
        saturating_increment(&s_adapter.stats.radar_upload_success_count);
    } else {
        s_adapter.retry_delay_ms = s_adapter.retry_delay_ms == 0U ? 100U :
            (s_adapter.retry_delay_ms >= 1000U ? 1000U : s_adapter.retry_delay_ms * 2U);
        s_adapter.next_radar_attempt_ms = now_ms + s_adapter.retry_delay_ms;
        s_adapter.force_radar_upload = true;
        saturating_increment(&s_adapter.stats.radar_upload_failure_count);
    }
    s_adapter.stats.latest_pending = s_adapter.has_latest &&
        s_adapter.latest_generation != s_adapter.inflight_generation;
    portEXIT_CRITICAL(&s_adapter_lock);
}

bool radar_resource_adapter_bme_upload_due(uint64_t now_ms)
{
#if !CONFIG_C5_BME_ADAPTIVE_REPORT
    return true;
#else
    radar_resource_adapter_tick(now_ms);
    portENTER_CRITICAL(&s_adapter_lock);
    const bool cooling_down = s_adapter.voice_recovery_until_ms != 0U &&
                              now_ms < s_adapter.voice_recovery_until_ms;
    const uint32_t period_ms = bme_upload_period(s_adapter.mode);
    const bool due = !s_adapter.voice_active && !cooling_down && period_ms != UINT32_MAX &&
                     (s_adapter.force_bme_upload || elapsed(now_ms, s_adapter.last_bme_upload_ms,
                                                            period_ms));
    portEXIT_CRITICAL(&s_adapter_lock);
    return due;
#endif
}

void radar_resource_adapter_complete_bme_upload(bool success, uint64_t now_ms)
{
#if CONFIG_C5_BME_ADAPTIVE_REPORT
    portENTER_CRITICAL(&s_adapter_lock);
    if (success) {
        s_adapter.last_bme_upload_ms = now_ms;
        s_adapter.force_bme_upload = false;
        saturating_increment(&s_adapter.stats.bme_upload_success_count);
    }
    portEXIT_CRITICAL(&s_adapter_lock);
#else
    (void)success;
    (void)now_ms;
#endif
}

uint32_t radar_resource_adapter_bme_event_period_ms(uint64_t now_ms)
{
    radar_resource_adapter_tick(now_ms);
    portENTER_CRITICAL(&s_adapter_lock);
    const bool recovery_due = !s_adapter.voice_active &&
        s_adapter.voice_recovery_until_ms != 0U && now_ms >= s_adapter.voice_recovery_until_ms &&
        s_adapter.force_bme_upload;
    const uint32_t period_ms = recovery_due ? C5_RADAR_VOICE_RECOVERY_MS :
                                               bme_upload_period(s_adapter.mode);
    portEXIT_CRITICAL(&s_adapter_lock);
    return period_ms;
}

void radar_resource_adapter_get_stats(radar_resource_adapter_stats_t *out)
{
    if (out == NULL) {
        return;
    }
    portENTER_CRITICAL(&s_adapter_lock);
    *out = s_adapter.stats;
    portEXIT_CRITICAL(&s_adapter_lock);
}
