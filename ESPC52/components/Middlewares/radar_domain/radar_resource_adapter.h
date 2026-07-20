#ifndef RADAR_RESOURCE_ADAPTER_H
#define RADAR_RESOURCE_ADAPTER_H

#include <stdbool.h>
#include <stdint.h>

#include "app_main_config.h"
#include "radar_target_sample.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef CONFIG_C5_RADAR_ADAPTIVE_UPLOAD
#define CONFIG_C5_RADAR_ADAPTIVE_UPLOAD 1
#endif

#ifndef CONFIG_C5_BME_ADAPTIVE_REPORT
#define CONFIG_C5_BME_ADAPTIVE_REPORT 1
#endif

#ifndef CONFIG_C5_VOICE_TELEMETRY_THROTTLE
#define CONFIG_C5_VOICE_TELEMETRY_THROTTLE 1
#endif

typedef enum {
    C5_SENSE_VACANT_STABLE = 0,
    C5_SENSE_VACANT_RECENT,
    C5_SENSE_PRESENT_STILL,
    C5_SENSE_PRESENT_MOVING,
    C5_SENSE_RADAR_DEGRADED,
    C5_SENSE_VOICE_ACTIVE,
} c5_sense_mode_t;

typedef struct {
    c5_sense_mode_t mode;
    uint32_t radar_generated_count;
    uint32_t radar_upload_attempt_count;
    uint32_t radar_upload_success_count;
    uint32_t radar_upload_failure_count;
    uint32_t radar_upload_coalesce_count;
    uint32_t radar_upload_stale_skip_count;
    uint32_t radar_state_transition_upload_count;
    uint32_t bme_upload_success_count;
    bool voice_active;
    bool latest_pending;
} radar_resource_adapter_stats_t;

void radar_resource_adapter_init(uint64_t now_ms);
void radar_resource_adapter_deinit(void);
void radar_resource_adapter_update_sample(const radar_target_sample_t *sample,
                                          uint64_t now_ms);
void radar_resource_adapter_set_link_state(uint8_t link_state,
                                           bool online,
                                           uint64_t now_ms);
void radar_resource_adapter_tick(uint64_t now_ms);

bool radar_resource_adapter_take_radar_upload(uint64_t now_ms,
                                              radar_target_sample_t *out_sample,
                                              uint32_t *out_sequence);
void radar_resource_adapter_complete_radar_upload(bool success, uint64_t now_ms);

bool radar_resource_adapter_bme_upload_due(uint64_t now_ms);
void radar_resource_adapter_complete_bme_upload(bool success, uint64_t now_ms);
uint32_t radar_resource_adapter_bme_event_period_ms(uint64_t now_ms);
void radar_resource_adapter_get_stats(radar_resource_adapter_stats_t *out);
const char *radar_resource_adapter_mode_name(c5_sense_mode_t mode);

#ifdef __cplusplus
}
#endif

#endif /* RADAR_RESOURCE_ADAPTER_H */
