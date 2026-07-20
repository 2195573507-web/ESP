#ifndef HOME_AI_USER_OVERRIDE_H
#define HOME_AI_USER_OVERRIDE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HOME_AI_MAX_USER_OVERRIDES 12U
#define HOME_AI_OVERRIDE_ID_LEN 64U
#define HOME_AI_OVERRIDE_ROOM_ID_LEN 32U
#define HOME_AI_OVERRIDE_DEVICE_ID_LEN 48U
#define HOME_AI_OVERRIDE_UNTIL_CONDITION_LEN 64U

typedef enum {
    HOME_AI_OVERRIDE_KEEP_ON = 0,
    HOME_AI_OVERRIDE_KEEP_OFF,
    HOME_AI_OVERRIDE_PAUSE_AUTOMATION,
    HOME_AI_OVERRIDE_MUTE,
} home_ai_override_action_t;

typedef struct {
    char override_id[HOME_AI_OVERRIDE_ID_LEN];
    char room_id[HOME_AI_OVERRIDE_ROOM_ID_LEN];
    char device_id[HOME_AI_OVERRIDE_DEVICE_ID_LEN];
    home_ai_override_action_t action;
    uint16_t priority;
    uint64_t created_at_ms;
    uint64_t expires_at_ms;
    char until_condition[HOME_AI_OVERRIDE_UNTIL_CONDITION_LEN];
    bool allow_safety_override;
} home_ai_user_override_t;

typedef enum {
    HOME_AI_OVERRIDE_DECISION_NONE = 0,
    HOME_AI_OVERRIDE_DECISION_SUPPRESS,
    HOME_AI_OVERRIDE_DECISION_MUTE,
} home_ai_override_decision_t;

bool home_ai_user_override_manager_init(void);
esp_err_t home_ai_user_override_upsert(const home_ai_user_override_t *override);
/* Replace only Server-managed overrides; local command overrides remain intact. */
esp_err_t home_ai_user_override_replace_synced(const home_ai_user_override_t *overrides,
                                               size_t count,
                                               uint64_t now_ms);
bool home_ai_user_override_remove(const char *override_id);
void home_ai_user_override_expire(uint64_t now_ms);
size_t home_ai_user_override_snapshot(home_ai_user_override_t *out, size_t capacity);

home_ai_override_decision_t home_ai_user_override_evaluate(const char *room_id,
                                                            const char *device_id,
                                                            const char *action,
                                                            bool safety_action,
                                                            uint64_t now_ms,
                                                            home_ai_user_override_t *out_match);
const char *home_ai_override_action_name(home_ai_override_action_t action);

#ifdef __cplusplus
}
#endif

#endif /* HOME_AI_USER_OVERRIDE_H */
