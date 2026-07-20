#ifndef HOME_AI_VIRTUAL_DEVICE_H
#define HOME_AI_VIRTUAL_DEVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "home_ai_rule_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HOME_AI_MAX_VIRTUAL_DEVICES 9U

typedef enum {
    HOME_AI_VIRTUAL_POWER_OFF = 0,
    HOME_AI_VIRTUAL_POWER_ON,
} home_ai_virtual_power_t;

typedef enum {
    HOME_AI_VIRTUAL_EXECUTION_APPLIED = 0,
    HOME_AI_VIRTUAL_EXECUTION_NOOP,
    HOME_AI_VIRTUAL_EXECUTION_DEFERRED_MINIMUM,
    HOME_AI_VIRTUAL_EXECUTION_REJECTED,
} home_ai_virtual_execution_result_t;

typedef struct {
    bool valid;
    char device_id[HOME_AI_RULE_DEVICE_ID_LEN];
    char room_id[HOME_AI_RULE_ROOM_ID_LEN];
    home_ai_rule_device_type_t device_type;
    home_ai_virtual_power_t power;
    char last_action[24];
    char action_source[48];
    char decision_id[HOME_AI_RULE_DECISION_ID_LEN];
    char decision_reason[96];
    bool verified;
    uint64_t updated_at_ms;
    uint64_t minimum_transition_at_ms;
} home_ai_virtual_device_state_t;

typedef struct {
    home_ai_virtual_execution_result_t result;
    home_ai_virtual_device_state_t state;
    char reason[64];
} home_ai_virtual_device_execution_t;

bool home_ai_virtual_device_executor_init(void);
esp_err_t home_ai_virtual_device_execute(const home_ai_rule_decision_t *decision,
                                         uint64_t now_ms,
                                         bool explicit_user_command,
                                         home_ai_virtual_device_execution_t *out);
bool home_ai_virtual_device_get(const char *device_id, home_ai_virtual_device_state_t *out);
size_t home_ai_virtual_device_snapshot(home_ai_virtual_device_state_t *out, size_t capacity);
const char *home_ai_virtual_power_name(home_ai_virtual_power_t power);
const char *home_ai_virtual_execution_result_name(home_ai_virtual_execution_result_t result);

#ifdef __cplusplus
}
#endif

#endif /* HOME_AI_VIRTUAL_DEVICE_H */
