#ifndef HOME_AI_LOCAL_VOICE_COMMAND_H
#define HOME_AI_LOCAL_VOICE_COMMAND_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "home_ai_room_state.h"
#include "home_ai_rule_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HOME_AI_LOCAL_VOICE_COMMAND_TEXT_LEN 96U

typedef enum {
    HOME_AI_LOCAL_VOICE_COMMAND_NONE = 0,
    HOME_AI_LOCAL_VOICE_COMMAND_STOP,
    HOME_AI_LOCAL_VOICE_COMMAND_CANCEL,
    HOME_AI_LOCAL_VOICE_COMMAND_MUTE,
    HOME_AI_LOCAL_VOICE_COMMAND_RESUME_SPEECH,
    HOME_AI_LOCAL_VOICE_COMMAND_PAUSE_AUTOMATION,
    HOME_AI_LOCAL_VOICE_COMMAND_RESUME_AUTOMATION,
    HOME_AI_LOCAL_VOICE_COMMAND_LIGHT_ON,
    HOME_AI_LOCAL_VOICE_COMMAND_LIGHT_OFF,
    HOME_AI_LOCAL_VOICE_COMMAND_KEEP_ON,
    HOME_AI_LOCAL_VOICE_COMMAND_KEEP_OFF,
    HOME_AI_LOCAL_VOICE_COMMAND_UNDO_LAST,
    HOME_AI_LOCAL_VOICE_COMMAND_DONT_DO_THAT,
} home_ai_local_voice_command_type_t;

typedef struct {
    home_ai_local_voice_command_type_t command;
    char room_id[HOME_AI_ROOM_STATE_ROOM_ID_LEN];
    char device_id[HOME_AI_RULE_DEVICE_ID_LEN];
} home_ai_local_voice_command_t;

typedef struct {
    bool applied;
    home_ai_local_voice_command_type_t command;
    char room_id[HOME_AI_ROOM_STATE_ROOM_ID_LEN];
    char device_id[HOME_AI_RULE_DEVICE_ID_LEN];
    char reason[64];
} home_ai_local_voice_command_result_t;

/* Parse only the fixed built-in vocabulary. Unknown/open-ended text is rejected. */
bool home_ai_local_voice_command_parse(const char *command_text,
                                       const char *default_room_id,
                                       home_ai_local_voice_command_t *out);

/* Execute through existing fixed-capacity S3 modules; this function owns no task or heap. */
esp_err_t home_ai_local_voice_command_execute(
    const home_ai_local_voice_command_t *command,
    uint64_t now_ms,
    home_ai_local_voice_command_result_t *out);

const char *home_ai_local_voice_command_name(home_ai_local_voice_command_type_t command);

#ifdef __cplusplus
}
#endif

#endif /* HOME_AI_LOCAL_VOICE_COMMAND_H */
