#include "home_ai_local_voice_command.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "home_ai_event_reporter.h"
#include "home_ai_user_override.h"
#include "home_ai_virtual_device.h"
#include "home_ai_voice_router.h"

#define HOME_AI_LOCAL_KEEP_DURATION_MS (2ULL * 60ULL * 60ULL * 1000ULL)
#define HOME_AI_LOCAL_OVERRIDE_PRIORITY 950U

static void copy_text(char *out, size_t out_size, const char *value)
{
    if (out == NULL || out_size == 0U) return;
    size_t length = 0U;
    if (value != NULL) {
        while (length + 1U < out_size && value[length] != '\0') ++length;
        memcpy(out, value, length);
    }
    out[length] = '\0';
}

static bool text_valid(const char *value, size_t capacity)
{
    if (value == NULL || value[0] == '\0') return false;
    for (size_t index = 0U; index < capacity; ++index) {
        if (value[index] == '\0') return true;
    }
    return false;
}

static bool normalize_text(const char *input, char *out, size_t out_size)
{
    if (input == NULL || out == NULL || out_size < 2U) return false;
    size_t used = 0U;
    for (size_t index = 0U; input[index] != '\0'; ++index) {
        const unsigned char value = (unsigned char)input[index];
        if (value < 0x20U || value == 0x7fU) return false;
        if (value < 0x80U && (isspace(value) || strchr(",.!?;:'\"-_", (int)value) != NULL)) {
            continue;
        }
        if (used + 1U >= out_size) return false;
        out[used++] = value < 0x80U ? (char)tolower(value) : (char)value;
    }
    out[used] = '\0';
    return used > 0U;
}

static bool equals_any(const char *value, const char *const *items, size_t count)
{
    for (size_t index = 0U; index < count; ++index) {
        if (strcmp(value, items[index]) == 0) return true;
    }
    return false;
}

typedef struct {
    const char *alias;
    const char *room_id;
} room_alias_t;

static void strip_prefix(char *value, const char *prefix)
{
    const size_t prefix_len = strlen(prefix);
    if (strncmp(value, prefix, prefix_len) == 0) {
        memmove(value, value + prefix_len, strlen(value + prefix_len) + 1U);
    }
}

static void strip_suffix(char *value, const char *suffix)
{
    const size_t value_len = strlen(value);
    const size_t suffix_len = strlen(suffix);
    if (value_len >= suffix_len && strcmp(value + value_len - suffix_len, suffix) == 0) {
        value[value_len - suffix_len] = '\0';
    }
}

static void strip_polite_affixes(char *value)
{
    static const char *const prefixes[] = {"请", "麻烦"};
    static const char *const suffixes[] = {"吧", "一下"};
    for (size_t index = 0U; index < sizeof(prefixes) / sizeof(prefixes[0]); ++index) {
        const size_t before = strlen(value);
        strip_prefix(value, prefixes[index]);
        if (strlen(value) != before) break;
    }
    for (size_t pass = 0U; pass < sizeof(suffixes) / sizeof(suffixes[0]); ++pass) {
        for (size_t index = 0U; index < sizeof(suffixes) / sizeof(suffixes[0]); ++index) {
            strip_suffix(value, suffixes[index]);
        }
    }
}

static bool split_room_and_command(const char *normalized,
                                   const char *fallback,
                                   char *out_room,
                                   size_t room_size,
                                   char *out_command,
                                   size_t command_size)
{
    static const room_alias_t aliases[] = {
        {.alias = "客厅", .room_id = "living_room"},
        {.alias = "livingroom", .room_id = "living_room"},
        {.alias = "卧室二", .room_id = "bedroom_02"},
        {.alias = "二号卧室", .room_id = "bedroom_02"},
        {.alias = "bedroom02", .room_id = "bedroom_02"},
        {.alias = "卧室一", .room_id = "bedroom_01"},
        {.alias = "一号卧室", .room_id = "bedroom_01"},
        {.alias = "bedroom01", .room_id = "bedroom_01"},
    };
    const room_alias_t *selected = NULL;
    const char *match = NULL;
    for (size_t index = 0U; index < sizeof(aliases) / sizeof(aliases[0]); ++index) {
        const char *candidate = strstr(normalized, aliases[index].alias);
        if (candidate == NULL) continue;
        if (selected != NULL || strstr(candidate + strlen(aliases[index].alias),
                                       aliases[index].alias) != NULL) {
            return false;
        }
        selected = &aliases[index];
        match = candidate;
    }

    if (selected == NULL) {
        copy_text(out_room, room_size, fallback);
        copy_text(out_command, command_size, normalized);
    } else {
        const size_t before = (size_t)(match - normalized);
        const size_t alias_len = strlen(selected->alias);
        const size_t after = strlen(match + alias_len);
        if (before + after + 1U > command_size) return false;
        memcpy(out_command, normalized, before);
        memcpy(out_command + before, match + alias_len, after + 1U);
        copy_text(out_room, room_size, selected->room_id);
    }
    strip_polite_affixes(out_command);
    return out_command[0] != '\0';
}

static bool command_needs_light(home_ai_local_voice_command_type_t command)
{
    return command == HOME_AI_LOCAL_VOICE_COMMAND_LIGHT_ON ||
           command == HOME_AI_LOCAL_VOICE_COMMAND_LIGHT_OFF ||
           command == HOME_AI_LOCAL_VOICE_COMMAND_KEEP_ON ||
           command == HOME_AI_LOCAL_VOICE_COMMAND_KEEP_OFF;
}

bool home_ai_local_voice_command_parse(const char *command_text,
                                       const char *default_room_id,
                                       home_ai_local_voice_command_t *out)
{
    if (out == NULL || !text_valid(command_text, HOME_AI_LOCAL_VOICE_COMMAND_TEXT_LEN) ||
        !text_valid(default_room_id, HOME_AI_ROOM_STATE_ROOM_ID_LEN)) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    char normalized[HOME_AI_LOCAL_VOICE_COMMAND_TEXT_LEN] = {0};
    if (!normalize_text(command_text, normalized, sizeof(normalized))) return false;
    char command_only[HOME_AI_LOCAL_VOICE_COMMAND_TEXT_LEN] = {0};
    if (!split_room_and_command(normalized,
                                default_room_id,
                                out->room_id,
                                sizeof(out->room_id),
                                command_only,
                                sizeof(command_only))) {
        return false;
    }

    static const char *const resume_automation[] = {"恢复自动控制", "恢复自动", "resumeautomation"};
    static const char *const pause_automation[] = {"暂停自动控制", "暂停自动", "pauseautomation"};
    static const char *const resume_speech[] = {"恢复播报", "解除静音", "resumespeech", "unmute"};
    static const char *const keep_on[] = {"保持打开", "先别关", "keepon"};
    static const char *const keep_off[] = {"保持关闭", "先别开", "keepoff"};
    static const char *const undo[] = {"撤销最近动作", "撤销刚才", "undolast"};
    static const char *const dont_do[] = {"不要这样做", "别这样做", "dontdothat"};
    static const char *const light_on[] = {"打开灯", "开灯", "turnonlight"};
    static const char *const light_off[] = {"关闭灯", "关灯", "turnofflight"};
    static const char *const cancel[] = {"取消", "cancel"};
    static const char *const stop[] = {"停止", "停下", "stop"};
    static const char *const mute[] = {"静音", "不要播报", "mute"};

#define MATCH(items) equals_any(command_only, items, sizeof(items) / sizeof((items)[0]))
    if (MATCH(resume_automation)) out->command = HOME_AI_LOCAL_VOICE_COMMAND_RESUME_AUTOMATION;
    else if (MATCH(pause_automation)) out->command = HOME_AI_LOCAL_VOICE_COMMAND_PAUSE_AUTOMATION;
    else if (MATCH(resume_speech)) out->command = HOME_AI_LOCAL_VOICE_COMMAND_RESUME_SPEECH;
    else if (MATCH(keep_on)) out->command = HOME_AI_LOCAL_VOICE_COMMAND_KEEP_ON;
    else if (MATCH(keep_off)) out->command = HOME_AI_LOCAL_VOICE_COMMAND_KEEP_OFF;
    else if (MATCH(undo)) out->command = HOME_AI_LOCAL_VOICE_COMMAND_UNDO_LAST;
    else if (MATCH(dont_do)) out->command = HOME_AI_LOCAL_VOICE_COMMAND_DONT_DO_THAT;
    else if (MATCH(light_on)) out->command = HOME_AI_LOCAL_VOICE_COMMAND_LIGHT_ON;
    else if (MATCH(light_off)) out->command = HOME_AI_LOCAL_VOICE_COMMAND_LIGHT_OFF;
    else if (MATCH(cancel)) out->command = HOME_AI_LOCAL_VOICE_COMMAND_CANCEL;
    else if (MATCH(stop)) out->command = HOME_AI_LOCAL_VOICE_COMMAND_STOP;
    else if (MATCH(mute)) out->command = HOME_AI_LOCAL_VOICE_COMMAND_MUTE;
#undef MATCH
    else return false;

    if (!text_valid(out->room_id, sizeof(out->room_id))) return false;
    if (command_needs_light(out->command)) {
        const int written = snprintf(out->device_id, sizeof(out->device_id), "%s_light", out->room_id);
        if (written <= 0 || written >= (int)sizeof(out->device_id)) return false;
    }
    return true;
}

static void result_init(home_ai_local_voice_command_result_t *out,
                        const home_ai_local_voice_command_t *command)
{
    if (out == NULL) return;
    memset(out, 0, sizeof(*out));
    out->command = command->command;
    copy_text(out->room_id, sizeof(out->room_id), command->room_id);
    copy_text(out->device_id, sizeof(out->device_id), command->device_id);
}

static esp_err_t apply_override(const home_ai_local_voice_command_t *command,
                                home_ai_override_action_t action,
                                const char *prefix,
                                uint64_t expires_at_ms,
                                uint64_t now_ms)
{
    home_ai_user_override_t override = {0};
    const int written = snprintf(override.override_id,
                                 sizeof(override.override_id),
                                 "%s_%s",
                                 prefix,
                                 command->room_id);
    if (written <= 0 || written >= (int)sizeof(override.override_id)) return ESP_ERR_INVALID_SIZE;
    copy_text(override.room_id, sizeof(override.room_id), command->room_id);
    copy_text(override.device_id, sizeof(override.device_id), command->device_id);
    override.action = action;
    override.priority = HOME_AI_LOCAL_OVERRIDE_PRIORITY;
    override.created_at_ms = now_ms;
    override.expires_at_ms = expires_at_ms;
    copy_text(override.until_condition, sizeof(override.until_condition), "offline_voice_resume");
    override.allow_safety_override = true;
    const esp_err_t ret = home_ai_user_override_upsert(&override);
    if (ret == ESP_OK) {
        home_ai_event_reporter_record_user_override(&override, command->device_id, now_ms);
    }
    return ret;
}

static bool remove_override(const char *prefix, const char *room_id)
{
    char override_id[HOME_AI_OVERRIDE_ID_LEN] = {0};
    const int written = snprintf(override_id, sizeof(override_id), "%s_%s", prefix, room_id);
    return written > 0 && written < (int)sizeof(override_id) &&
           home_ai_user_override_remove(override_id);
}

static esp_err_t execute_virtual(const char *room_id,
                                 const char *device_id,
                                 home_ai_rule_device_type_t device_type,
                                 home_ai_rule_action_t action,
                                 uint64_t now_ms,
                                 home_ai_local_voice_command_result_t *out)
{
    home_ai_rule_decision_t decision = {0};
    (void)snprintf(decision.decision_id,
                   sizeof(decision.decision_id),
                   "offline_%llu",
                   (unsigned long long)now_ms);
    copy_text(decision.rule_id, sizeof(decision.rule_id), "offline_voice_command");
    copy_text(decision.room_id, sizeof(decision.room_id), room_id);
    copy_text(decision.device_id, sizeof(decision.device_id), device_id);
    decision.device_type = device_type;
    decision.action = action;
    decision.state = HOME_AI_RULE_DECISION_EXECUTE;
    decision.priority = 950U;
    home_ai_virtual_device_execution_t execution = {0};
    const esp_err_t ret = home_ai_virtual_device_execute(&decision, now_ms, true, &execution);
    if (ret == ESP_OK) {
        home_ai_event_reporter_record_decision(&decision, &execution, now_ms);
        home_ai_event_reporter_record_virtual_state(&execution.state, now_ms);
        if (out != NULL) {
            out->applied = execution.result == HOME_AI_VIRTUAL_EXECUTION_APPLIED ||
                           execution.result == HOME_AI_VIRTUAL_EXECUTION_NOOP;
            copy_text(out->device_id, sizeof(out->device_id), execution.state.device_id);
            copy_text(out->reason, sizeof(out->reason), execution.reason);
        }
    }
    return ret;
}

static esp_err_t undo_last(const home_ai_local_voice_command_t *command,
                           uint64_t now_ms,
                           home_ai_local_voice_command_result_t *out)
{
    home_ai_virtual_device_state_t states[HOME_AI_MAX_VIRTUAL_DEVICES] = {0};
    const size_t count = home_ai_virtual_device_snapshot(states, HOME_AI_MAX_VIRTUAL_DEVICES);
    const home_ai_virtual_device_state_t *latest = NULL;
    for (size_t index = 0U; index < count; ++index) {
        if (strcmp(states[index].room_id, command->room_id) != 0) continue;
        if (latest == NULL || states[index].updated_at_ms > latest->updated_at_ms) latest = &states[index];
    }
    if (latest == NULL) return ESP_ERR_NOT_FOUND;
    return execute_virtual(latest->room_id,
                           latest->device_id,
                           latest->device_type,
                           latest->power == HOME_AI_VIRTUAL_POWER_ON ?
                               HOME_AI_RULE_ACTION_TURN_OFF : HOME_AI_RULE_ACTION_TURN_ON,
                           now_ms,
                           out);
}

esp_err_t home_ai_local_voice_command_execute(
    const home_ai_local_voice_command_t *command,
    uint64_t now_ms,
    home_ai_local_voice_command_result_t *out)
{
    if (command == NULL || out == NULL || now_ms == 0U ||
        command->command <= HOME_AI_LOCAL_VOICE_COMMAND_NONE ||
        command->command > HOME_AI_LOCAL_VOICE_COMMAND_DONT_DO_THAT ||
        !text_valid(command->room_id, sizeof(command->room_id))) {
        return ESP_ERR_INVALID_ARG;
    }
    result_init(out, command);
    esp_err_t ret = ESP_ERR_NOT_SUPPORTED;
    switch (command->command) {
    case HOME_AI_LOCAL_VOICE_COMMAND_STOP:
    case HOME_AI_LOCAL_VOICE_COMMAND_CANCEL:
        ret = home_ai_voice_router_request_stop(command->room_id);
        break;
    case HOME_AI_LOCAL_VOICE_COMMAND_MUTE:
        if (!home_ai_voice_router_set_mute(command->room_id, true, UINT64_MAX)) {
            ret = ESP_FAIL;
            break;
        }
        ret = apply_override(command,
                             HOME_AI_OVERRIDE_MUTE,
                             "offline_mute",
                             0U,
                             now_ms);
        if (ret != ESP_OK) (void)home_ai_voice_router_set_mute(command->room_id, false, 0U);
        break;
    case HOME_AI_LOCAL_VOICE_COMMAND_RESUME_SPEECH:
        (void)remove_override("offline_mute", command->room_id);
        ret = home_ai_voice_router_set_mute(command->room_id, false, 0U) ? ESP_OK : ESP_FAIL;
        break;
    case HOME_AI_LOCAL_VOICE_COMMAND_PAUSE_AUTOMATION:
        ret = apply_override(command,
                             HOME_AI_OVERRIDE_PAUSE_AUTOMATION,
                             "offline_pause",
                             0U,
                             now_ms);
        break;
    case HOME_AI_LOCAL_VOICE_COMMAND_RESUME_AUTOMATION:
        (void)remove_override("offline_pause", command->room_id);
        (void)remove_override("offline_keep", command->room_id);
        ret = ESP_OK;
        break;
    case HOME_AI_LOCAL_VOICE_COMMAND_LIGHT_ON:
    case HOME_AI_LOCAL_VOICE_COMMAND_LIGHT_OFF:
        ret = execute_virtual(command->room_id,
                              command->device_id,
                              HOME_AI_RULE_DEVICE_LIGHT,
                              command->command == HOME_AI_LOCAL_VOICE_COMMAND_LIGHT_ON ?
                                  HOME_AI_RULE_ACTION_TURN_ON : HOME_AI_RULE_ACTION_TURN_OFF,
                              now_ms,
                              out);
        break;
    case HOME_AI_LOCAL_VOICE_COMMAND_KEEP_ON:
    case HOME_AI_LOCAL_VOICE_COMMAND_KEEP_OFF:
        ret = execute_virtual(command->room_id,
                              command->device_id,
                              HOME_AI_RULE_DEVICE_LIGHT,
                              command->command == HOME_AI_LOCAL_VOICE_COMMAND_KEEP_ON ?
                                  HOME_AI_RULE_ACTION_TURN_ON : HOME_AI_RULE_ACTION_TURN_OFF,
                              now_ms,
                              out);
        if (ret == ESP_OK) {
            ret = apply_override(command,
                                 command->command == HOME_AI_LOCAL_VOICE_COMMAND_KEEP_ON ?
                                     HOME_AI_OVERRIDE_KEEP_ON : HOME_AI_OVERRIDE_KEEP_OFF,
                                 "offline_keep",
                                 now_ms + HOME_AI_LOCAL_KEEP_DURATION_MS,
                                 now_ms);
        }
        break;
    case HOME_AI_LOCAL_VOICE_COMMAND_UNDO_LAST:
    case HOME_AI_LOCAL_VOICE_COMMAND_DONT_DO_THAT:
        ret = undo_last(command, now_ms, out);
        break;
    case HOME_AI_LOCAL_VOICE_COMMAND_NONE:
    default:
        ret = ESP_ERR_NOT_SUPPORTED;
        break;
    }
    if (ret == ESP_OK) {
        out->applied = true;
        if (out->reason[0] == '\0') copy_text(out->reason, sizeof(out->reason), "offline_voice_command_applied");
    } else if (out->reason[0] == '\0') {
        copy_text(out->reason, sizeof(out->reason), "offline_voice_command_failed");
    }
    return ret;
}

const char *home_ai_local_voice_command_name(home_ai_local_voice_command_type_t command)
{
    switch (command) {
    case HOME_AI_LOCAL_VOICE_COMMAND_STOP: return "stop";
    case HOME_AI_LOCAL_VOICE_COMMAND_CANCEL: return "cancel";
    case HOME_AI_LOCAL_VOICE_COMMAND_MUTE: return "mute";
    case HOME_AI_LOCAL_VOICE_COMMAND_RESUME_SPEECH: return "resume_speech";
    case HOME_AI_LOCAL_VOICE_COMMAND_PAUSE_AUTOMATION: return "pause_automation";
    case HOME_AI_LOCAL_VOICE_COMMAND_RESUME_AUTOMATION: return "resume_automation";
    case HOME_AI_LOCAL_VOICE_COMMAND_LIGHT_ON: return "light_on";
    case HOME_AI_LOCAL_VOICE_COMMAND_LIGHT_OFF: return "light_off";
    case HOME_AI_LOCAL_VOICE_COMMAND_KEEP_ON: return "keep_on";
    case HOME_AI_LOCAL_VOICE_COMMAND_KEEP_OFF: return "keep_off";
    case HOME_AI_LOCAL_VOICE_COMMAND_UNDO_LAST: return "undo_last";
    case HOME_AI_LOCAL_VOICE_COMMAND_DONT_DO_THAT: return "dont_do_that";
    case HOME_AI_LOCAL_VOICE_COMMAND_NONE:
    default: return "unknown";
    }
}
