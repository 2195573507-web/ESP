#ifndef RADAR_PERSON_CONTINUITY_H
#define RADAR_PERSON_CONTINUITY_H

#include <stddef.h>

#include "radar_spatial_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t track_id;
    uint64_t last_seen_ms;
    uint8_t consecutive_confirmed_frames;
    bool near_retained_person;
} radar_person_candidate_t;

typedef struct {
    radar_person_continuity_config_t config;
    radar_person_snapshot_t persons[RADAR_PERSON_CONTINUITY_MAX_PERSONS];
    radar_person_candidate_t candidates[RADAR_PERSON_CONTINUITY_MAX_CANDIDATES];
    uint32_t next_person_id;
    uint8_t source_id;
    uint32_t source_sequence;
    bool source_valid;
    bool unresolved_observation;
    uint8_t visible_person_count;
    uint8_t retained_person_count;
    uint8_t source_person_count;
    radar_person_count_state_t count_state;
    uint64_t last_mismatch_log_ms;
} radar_person_continuity_t;

void radar_person_continuity_init(radar_person_continuity_t *continuity,
                                  const radar_person_continuity_config_t *config,
                                  uint8_t source_id);
void radar_person_continuity_set_source(radar_person_continuity_t *continuity,
                                        uint8_t source_id);
void radar_person_continuity_set_sequence(radar_person_continuity_t *continuity,
                                          uint32_t sequence);
void radar_person_continuity_update(radar_person_continuity_t *continuity,
                                    const radar_track_snapshot_t *tracks,
                                    size_t track_count,
                                    bool source_valid,
                                    bool unresolved_observation,
                                    uint64_t now_ms);
void radar_person_continuity_copy_persons(const radar_person_continuity_t *continuity,
                                          radar_person_snapshot_t *out,
                                          size_t capacity);
void radar_person_continuity_get_counts(const radar_person_continuity_t *continuity,
                                        uint8_t *visible_person_count,
                                        uint8_t *retained_person_count,
                                        uint8_t *source_person_count,
                                        radar_person_count_state_t *count_state);
const char *radar_person_state_name(radar_person_state_t state);
const char *radar_person_count_state_name(radar_person_count_state_t state);

#ifdef __cplusplus
}
#endif

#endif
