#ifndef RADAR_SPATIAL_STATE_H
#define RADAR_SPATIAL_STATE_H

#include "radar_coordinate_transform.h"
#include "radar_person_continuity.h"
#include "radar_target_tracker.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* 状态机保存配置、轨迹和最新快照，调用者通过 get_snapshot 读取副本。 */
    radar_spatial_config_t config;
    radar_zone_map_t zone_map;
    radar_target_tracker_t tracker;
    radar_person_continuity_t person_continuity;
    radar_spatial_snapshot_t snapshot;
    uint32_t last_frame_seq;
    uint32_t motion_enter_streak;
    uint32_t motion_exit_streak;
    bool motion_confirmed;
    uint8_t history_write_index;
} radar_spatial_state_t;

radar_spatial_config_t radar_spatial_default_config(void);
void radar_spatial_state_init(radar_spatial_state_t *state,
                              const radar_spatial_config_t *config,
                              uint64_t now_ms);
void radar_spatial_state_set_source(radar_spatial_state_t *state, uint8_t source_id);
void radar_spatial_state_on_frame(radar_spatial_state_t *state,
                                  const radar_frame_t *frame,
                                  bool uart_recovered,
                                  uint64_t now_ms);
void radar_spatial_state_poll(radar_spatial_state_t *state,
                             radar_uart_recovery_state_t recovery_state,
                             uint64_t now_ms);
void radar_spatial_state_set_diagnostics(radar_spatial_state_t *state,
                                         const ld2450_parser_diagnostics_t *parser,
                                         const ld2450_uart_diagnostics_t *uart,
                                         const radar_uart_recovery_t *recovery);
void radar_spatial_state_get_snapshot(const radar_spatial_state_t *state,
                                      radar_spatial_snapshot_t *out);

#ifdef __cplusplus
}
#endif

#endif
