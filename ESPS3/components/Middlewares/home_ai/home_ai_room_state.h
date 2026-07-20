#ifndef HOME_AI_ROOM_STATE_H
#define HOME_AI_ROOM_STATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "radar_registry.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HOME_AI_ROOM_STATE_COUNT RADAR_SOURCE_COUNT
#define HOME_AI_ROOM_STATE_ROOM_ID_LEN 32U
#define HOME_AI_ROOM_STATE_VOICE_TERMINAL_ID_LEN 48U

typedef enum {
    HOME_AI_ROOM_PRESENCE_UNKNOWN = 0,
    HOME_AI_ROOM_PRESENCE_OCCUPIED,
    HOME_AI_ROOM_PRESENCE_VACANT,
} home_ai_room_presence_state_t;

typedef enum {
    HOME_AI_ROOM_OCCUPANCY_UNKNOWN = 0,
    HOME_AI_ROOM_OCCUPANCY_SINGLE,
    HOME_AI_ROOM_OCCUPANCY_MULTIPLE,
} home_ai_room_occupancy_mode_t;

typedef enum {
    HOME_AI_ROOM_QUIET_NORMAL = 0,
    HOME_AI_ROOM_QUIET_SCHEDULED,
    HOME_AI_ROOM_QUIET_SLEEP_CONFIRMED,
    HOME_AI_ROOM_QUIET_TEMPORARY_AWAKE,
} home_ai_room_quiet_state_t;

typedef enum {
    HOME_AI_ROOM_SCENE_NONE = 0,
    HOME_AI_ROOM_SCENE_WAKE_CANDIDATE,
    HOME_AI_ROOM_SCENE_WAKE_CONFIRMED,
    HOME_AI_ROOM_SCENE_LEAVE_CANDIDATE,
    HOME_AI_ROOM_SCENE_OTHER,
} home_ai_room_scene_state_t;

typedef enum {
    HOME_AI_ROOM_AUTOMATION_BASIC_ONLY = 0,
    HOME_AI_ROOM_AUTOMATION_SHARED_CONSERVATIVE,
    HOME_AI_ROOM_AUTOMATION_PERSONALIZED,
} home_ai_room_automation_profile_t;

typedef struct {
    radar_source_id_t source;
    char room_id[HOME_AI_ROOM_STATE_ROOM_ID_LEN];
    char voice_terminal_device_id[HOME_AI_ROOM_STATE_VOICE_TERMINAL_ID_LEN];
    uint32_t occupied_confirm_ms;
    uint32_t vacant_confirm_ms;
    uint32_t multiple_confirm_ms;
    uint32_t single_confirm_ms;
    uint16_t quiet_start_minute;
    uint16_t quiet_end_minute;
} home_ai_room_state_config_t;

typedef struct {
    radar_source_id_t source;
    char room_id[HOME_AI_ROOM_STATE_ROOM_ID_LEN];
    home_ai_room_presence_state_t presence_state;
    uint8_t stable_target_count;
    home_ai_room_occupancy_mode_t occupancy_mode;
    bool environment_fresh;
    bool radar_fresh;
    home_ai_room_quiet_state_t quiet_state;
    home_ai_room_scene_state_t scene_state;
    home_ai_room_automation_profile_t automation_profile;
    uint64_t last_state_change_ms;
} home_ai_room_state_t;

/* The state engine has one fixed record per radar source and owns no task or heap memory. */
bool home_ai_room_state_init(void);

/* Replace all three room/source mappings atomically after validating fixed resource bounds. */
bool home_ai_room_state_set_config(const home_ai_room_state_config_t *configs, size_t count);
bool home_ai_room_state_get_config(radar_source_id_t source, home_ai_room_state_config_t *out);

/* Called from s3_scheduler_tick() with a single radar registry snapshot. */
void home_ai_room_state_update(const radar_registry_entry_t *entries,
                               size_t count,
                               uint64_t now_ms);

/* BME integration sets freshness independently; it must never infer radar presence. */
bool home_ai_room_state_set_environment_fresh(radar_source_id_t source, bool fresh, uint64_t now_ms);
bool home_ai_room_state_set_quiet_state(radar_source_id_t source,
                                        home_ai_room_quiet_state_t quiet_state,
                                        uint64_t now_ms);
/* Scheduled quiet updates never override sleep-confirmed or temporary-awake state. */
bool home_ai_room_state_apply_quiet_schedule(radar_source_id_t source,
                                             bool scheduled,
                                             uint64_t now_ms);
bool home_ai_room_state_get(radar_source_id_t source, home_ai_room_state_t *out);
size_t home_ai_room_state_snapshot(home_ai_room_state_t *out, size_t capacity);

const char *home_ai_room_presence_state_name(home_ai_room_presence_state_t state);
const char *home_ai_room_occupancy_mode_name(home_ai_room_occupancy_mode_t mode);

#ifdef __cplusplus
}
#endif

#endif /* HOME_AI_ROOM_STATE_H */
