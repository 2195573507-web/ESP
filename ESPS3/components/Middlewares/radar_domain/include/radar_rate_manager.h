#ifndef RADAR_RATE_MANAGER_H
#define RADAR_RATE_MANAGER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RADAR_RATE_IDLE = 0,
    RADAR_RATE_DETECTING,
    RADAR_RATE_TRACKING,
    RADAR_RATE_FAST_MOVING,
    RADAR_RATE_LOST_PENDING,
    RADAR_RATE_LOST,
} radar_rate_mode_t;

typedef struct {
    uint32_t parser_period_ms;
    uint32_t tracker_period_ms;
    uint32_t snapshot_period_ms;
    uint32_t state_log_period_ms;
    uint32_t track_log_period_ms;
} radar_rate_policy_t;

typedef struct {
    radar_rate_mode_t mode;
    radar_rate_policy_t policy;
    uint64_t idle_since_ms;
    uint8_t previous_retained_count;
} radar_rate_manager_t;

void radar_rate_manager_init(radar_rate_manager_t *manager, uint64_t now_ms);
bool radar_rate_manager_update(radar_rate_manager_t *manager,
                               uint8_t candidate_count,
                               uint8_t active_count,
                               uint8_t retained_count,
                               uint64_t now_ms);
const char *radar_rate_manager_mode_name(radar_rate_mode_t mode);

#ifdef __cplusplus
}
#endif

#endif
