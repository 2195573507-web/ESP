#ifndef HOME_AI_RULE_ENGINE_H
#define HOME_AI_RULE_ENGINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "home_ai_room_state.h"
#include "home_ai_user_override.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HOME_AI_RULE_SCHEMA_VERSION 1U
#define HOME_AI_MAX_RULES 16U
#define HOME_AI_MAX_CONDITIONS_PER_RULE 8U
#define HOME_AI_MAX_ACTIONS_PER_RULE 4U
#define HOME_AI_MAX_RULE_STEPS 4U
#define HOME_AI_MAX_PENDING_DECISIONS 16U
#define HOME_AI_RULE_PACKAGE_BYTES 12288U
#define HOME_AI_RULE_STRING_POOL_BYTES HOME_AI_RULE_PACKAGE_BYTES
#define HOME_AI_RULE_ID_LEN 64U
#define HOME_AI_RULE_ROOM_ID_LEN HOME_AI_ROOM_STATE_ROOM_ID_LEN
#define HOME_AI_RULE_DEVICE_ID_LEN 48U
#define HOME_AI_RULE_PROMPT_LEN 96U
#define HOME_AI_RULE_DECISION_ID_LEN 64U

typedef enum {
    HOME_AI_RULE_DEVICE_LIGHT = 0,
    HOME_AI_RULE_DEVICE_AIR_CONDITIONER,
    HOME_AI_RULE_DEVICE_FAN,
} home_ai_rule_device_type_t;

typedef enum {
    HOME_AI_RULE_ACTION_TURN_ON = 0,
    HOME_AI_RULE_ACTION_TURN_OFF,
    HOME_AI_RULE_ACTION_PAUSE_AUTOMATION,
    HOME_AI_RULE_ACTION_RESUME_AUTOMATION,
    HOME_AI_RULE_ACTION_PLAY_PROMPT,
} home_ai_rule_action_t;

typedef enum {
    HOME_AI_RULE_DECISION_EXECUTE = 0,
    HOME_AI_RULE_DECISION_SUPPRESSED_OVERRIDE,
    HOME_AI_RULE_DECISION_SUPPRESSED_PRIORITY,
    HOME_AI_RULE_DECISION_SUPPRESSED_MUTE,
    HOME_AI_RULE_DECISION_SUPPRESSED_UNKNOWN_PRESENCE,
} home_ai_rule_decision_state_t;

typedef enum {
    HOME_AI_RULE_ACTIVATION_EMPTY = 0,
    HOME_AI_RULE_ACTIVATION_ACTIVE,
    HOME_AI_RULE_ACTIVATION_ACTIVE_PARTIAL,
    HOME_AI_RULE_ACTIVATION_REJECTED,
} home_ai_rule_activation_state_t;

typedef struct {
    bool valid;
    float temperature_c;
    float humidity_percent;
    float air_quality_score;
} home_ai_rule_environment_t;

typedef struct {
    const home_ai_room_state_t *rooms;
    size_t room_count;
    const home_ai_rule_environment_t *environment;
    size_t environment_count;
    const char *time_window;
    bool server_online;
    bool weather_available;
    bool weather_dark;
    uint64_t now_ms;
} home_ai_rule_evaluation_context_t;

typedef struct {
    char decision_id[HOME_AI_RULE_DECISION_ID_LEN];
    char rule_id[HOME_AI_RULE_ID_LEN];
    char room_id[HOME_AI_RULE_ROOM_ID_LEN];
    char device_id[HOME_AI_RULE_DEVICE_ID_LEN];
    char prompt[HOME_AI_RULE_PROMPT_LEN];
    home_ai_rule_device_type_t device_type;
    home_ai_rule_action_t action;
    home_ai_rule_decision_state_t state;
    uint16_t priority;
    uint32_t minimum_active_seconds;
    bool safety_action;
    char suppression_override_id[HOME_AI_OVERRIDE_ID_LEN];
    uint8_t rule_slot;
} home_ai_rule_decision_t;

typedef struct {
    char rule_id[HOME_AI_RULE_ID_LEN];
    bool accepted;
    bool retained_previous;
    char code[32];
} home_ai_rule_activation_item_t;

typedef struct {
    home_ai_rule_activation_state_t state;
    uint32_t package_version;
    uint32_t active_rule_count;
    uint32_t accepted_count;
    uint32_t rejected_count;
    home_ai_rule_activation_item_t items[HOME_AI_MAX_RULES];
    size_t item_count;
    char error_code[32];
} home_ai_rule_activation_result_t;

bool home_ai_rule_engine_init(void);
/* Payload is the checksum-verified, canonical Server package body, not an HTTP envelope. */
esp_err_t home_ai_rule_engine_apply_payload(const char *payload,
                                            size_t payload_len,
                                            home_ai_rule_activation_result_t *out_result);
bool home_ai_rule_engine_rollback(home_ai_rule_activation_result_t *out_result);
void home_ai_rule_engine_reset(void);
size_t home_ai_rule_engine_evaluate(const home_ai_rule_evaluation_context_t *context,
                                    home_ai_rule_decision_t *out,
                                    size_t capacity);
void home_ai_rule_engine_note_action_result(const home_ai_rule_decision_t *decision, bool success);
bool home_ai_rule_engine_get_activation(home_ai_rule_activation_result_t *out_result);

const char *home_ai_rule_action_name(home_ai_rule_action_t action);
const char *home_ai_rule_device_type_name(home_ai_rule_device_type_t type);
const char *home_ai_rule_activation_state_name(home_ai_rule_activation_state_t state);
const char *home_ai_rule_decision_state_name(home_ai_rule_decision_state_t state);

#ifdef __cplusplus
}
#endif

#endif /* HOME_AI_RULE_ENGINE_H */
