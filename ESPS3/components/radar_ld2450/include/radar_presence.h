#ifndef RADAR_PRESENCE_H
#define RADAR_PRESENCE_H

#include <stdbool.h>
#include <stdint.h>

#include "ld2450_types.h"
#include "radar_config.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RADAR_FRAME_PERIOD_EXPECTED_MS RADAR_CONFIG_FRAME_PERIOD_EXPECTED_MS
#define RADAR_ONLINE_TIMEOUT_MS RADAR_CONFIG_ONLINE_TIMEOUT_MS
#define RADAR_ENTER_WINDOW_FRAMES RADAR_CONFIG_ENTER_WINDOW_FRAMES
#define RADAR_ENTER_REQUIRED_FRAMES RADAR_CONFIG_ENTER_REQUIRED_FRAMES
#define RADAR_SHORT_GAP_MS RADAR_CONFIG_SHORT_GAP_MS
#define RADAR_HOLD_TIMEOUT_MS RADAR_CONFIG_HOLD_TIMEOUT_MS
#define RADAR_REPORT_HEARTBEAT_MS RADAR_CONFIG_REPORT_HEARTBEAT_MS
#define RADAR_TARGET_REPORT_MIN_INTERVAL_MS RADAR_CONFIG_TARGET_REPORT_MIN_INTERVAL_MS

typedef struct {
    uint32_t frame_period_expected_ms;
    uint32_t online_timeout_ms;
    uint8_t enter_window_frames;
    uint8_t enter_required_frames;
    uint32_t short_gap_ms;
    uint32_t hold_timeout_ms;
} radar_presence_config_t;

typedef struct {
    uint32_t time_rollback_count;
    uint32_t uart_error_count;
    uint32_t state_change_count;
} radar_presence_diagnostics_t;

typedef struct {
    radar_presence_config_t config;
    radar_snapshot_t snapshot;
    bool motion_history[RADAR_ENTER_WINDOW_FRAMES];
    uint8_t history_count;
    uint8_t history_index;
    uint32_t empty_frame_streak;
    uint64_t last_observed_ms;
    bool has_observed_time;
    radar_presence_diagnostics_t diagnostics;
} radar_presence_t;

radar_presence_config_t radar_presence_default_config(void);
void radar_presence_init(radar_presence_t *presence,
                         const radar_presence_config_t *config,
                         uint64_t now_ms);
void radar_presence_on_frame(radar_presence_t *presence,
                             const radar_frame_t *frame,
                             bool uart_online,
                             uint64_t now_ms);
void radar_presence_poll(radar_presence_t *presence,
                         bool uart_online,
                         uint64_t now_ms);
void radar_presence_note_uart_error(radar_presence_t *presence, uint64_t now_ms);
void radar_presence_get_snapshot(const radar_presence_t *presence,
                                 radar_snapshot_t *out);
void radar_presence_get_diagnostics(const radar_presence_t *presence,
                                    radar_presence_diagnostics_t *out);
const char *radar_presence_state_name(radar_presence_state_t state);

#ifdef __cplusplus
}
#endif

#endif
