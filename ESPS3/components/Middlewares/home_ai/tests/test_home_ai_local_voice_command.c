#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "home_ai_event_reporter.h"
#include "home_ai_local_voice_command.h"
#include "home_ai_user_override.h"
#include "home_ai_virtual_device.h"
#include "home_ai_voice_router.h"

static esp_err_t s_stop_result;
static bool s_mute_result;
static bool s_muted;
static unsigned int s_mute_calls;
static unsigned int s_decision_events;
static unsigned int s_state_events;
static unsigned int s_override_events;

esp_err_t home_ai_voice_router_request_stop(const char *room_id)
{
    assert(room_id != NULL && room_id[0] != '\0');
    return s_stop_result;
}

bool home_ai_voice_router_set_mute(const char *room_id, bool muted, uint64_t until_ms)
{
    assert(room_id != NULL && room_id[0] != '\0');
    (void)until_ms;
    ++s_mute_calls;
    if (!s_mute_result) return false;
    s_muted = muted;
    return true;
}

void home_ai_event_reporter_record_decision(const home_ai_rule_decision_t *decision,
                                            const home_ai_virtual_device_execution_t *execution,
                                            uint64_t occurred_at_ms)
{
    assert(decision != NULL && execution != NULL && occurred_at_ms > 0U);
    ++s_decision_events;
}

void home_ai_event_reporter_record_virtual_state(const home_ai_virtual_device_state_t *state,
                                                 uint64_t occurred_at_ms)
{
    assert(state != NULL && state->valid && occurred_at_ms > 0U);
    ++s_state_events;
}

void home_ai_event_reporter_record_user_override(const home_ai_user_override_t *override,
                                                 const char *device_id,
                                                 uint64_t occurred_at_ms)
{
    assert(override != NULL && device_id != NULL && occurred_at_ms > 0U);
    ++s_override_events;
}

static void reset_fixture(void)
{
    assert(home_ai_user_override_manager_init());
    assert(home_ai_virtual_device_executor_init());
    s_stop_result = ESP_OK;
    s_mute_result = true;
    s_muted = false;
    s_mute_calls = 0U;
    s_decision_events = 0U;
    s_state_events = 0U;
    s_override_events = 0U;
}

static home_ai_local_voice_command_t parse(const char *text, const char *room_id)
{
    home_ai_local_voice_command_t command = {0};
    assert(home_ai_local_voice_command_parse(text, room_id, &command));
    return command;
}

static bool has_override(const char *override_id, home_ai_override_action_t action)
{
    home_ai_user_override_t overrides[HOME_AI_MAX_USER_OVERRIDES] = {0};
    const size_t count = home_ai_user_override_snapshot(overrides, HOME_AI_MAX_USER_OVERRIDES);
    for (size_t index = 0U; index < count; ++index) {
        if (strcmp(overrides[index].override_id, override_id) == 0 &&
            overrides[index].action == action) {
            return true;
        }
    }
    return false;
}

static void test_fixed_vocabulary_and_room_selection(void)
{
    static const struct {
        const char *text;
        home_ai_local_voice_command_type_t command;
    } cases[] = {
        {"停止", HOME_AI_LOCAL_VOICE_COMMAND_STOP},
        {"取消", HOME_AI_LOCAL_VOICE_COMMAND_CANCEL},
        {"静音", HOME_AI_LOCAL_VOICE_COMMAND_MUTE},
        {"恢复播报", HOME_AI_LOCAL_VOICE_COMMAND_RESUME_SPEECH},
        {"暂停自动控制", HOME_AI_LOCAL_VOICE_COMMAND_PAUSE_AUTOMATION},
        {"恢复自动控制", HOME_AI_LOCAL_VOICE_COMMAND_RESUME_AUTOMATION},
        {"打开灯", HOME_AI_LOCAL_VOICE_COMMAND_LIGHT_ON},
        {"关闭灯", HOME_AI_LOCAL_VOICE_COMMAND_LIGHT_OFF},
        {"保持打开", HOME_AI_LOCAL_VOICE_COMMAND_KEEP_ON},
        {"保持关闭", HOME_AI_LOCAL_VOICE_COMMAND_KEEP_OFF},
        {"撤销最近动作", HOME_AI_LOCAL_VOICE_COMMAND_UNDO_LAST},
        {"不要这样做", HOME_AI_LOCAL_VOICE_COMMAND_DONT_DO_THAT},
    };
    for (size_t index = 0U; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        const home_ai_local_voice_command_t command = parse(cases[index].text, "bedroom_01");
        assert(command.command == cases[index].command);
        assert(strcmp(command.room_id, "bedroom_01") == 0);
        assert(strcmp(home_ai_local_voice_command_name(command.command), "unknown") != 0);
    }

    home_ai_local_voice_command_t command = parse("请客厅开灯一下吧", "bedroom_01");
    assert(command.command == HOME_AI_LOCAL_VOICE_COMMAND_LIGHT_ON);
    assert(strcmp(command.room_id, "living_room") == 0);
    assert(strcmp(command.device_id, "living_room_light") == 0);

    command = parse("bedroom02 turn on light", "bedroom_01");
    assert(command.command == HOME_AI_LOCAL_VOICE_COMMAND_LIGHT_ON);
    assert(strcmp(command.room_id, "bedroom_02") == 0);
    assert(strcmp(command.device_id, "bedroom_02_light") == 0);
}

static void test_rejects_open_ended_and_ambiguous_text(void)
{
    home_ai_local_voice_command_t command = {0};
    assert(!home_ai_local_voice_command_parse("不要打开灯", "bedroom_01", &command));
    assert(!home_ai_local_voice_command_parse("今天天气怎么样", "bedroom_01", &command));
    assert(!home_ai_local_voice_command_parse("客厅卧室一开灯", "bedroom_01", &command));
    assert(!home_ai_local_voice_command_parse("停止\n聊天", "bedroom_01", &command));
    assert(!home_ai_local_voice_command_parse("开灯", "", &command));

    char too_long[HOME_AI_LOCAL_VOICE_COMMAND_TEXT_LEN + 1U];
    memset(too_long, 'x', sizeof(too_long));
    too_long[sizeof(too_long) - 1U] = '\0';
    assert(!home_ai_local_voice_command_parse(too_long, "bedroom_01", &command));
}

static void test_stop_and_mute_failure_rollback(void)
{
    reset_fixture();
    home_ai_local_voice_command_result_t result = {0};
    home_ai_local_voice_command_t command = parse("停止", "bedroom_01");
    assert(home_ai_local_voice_command_execute(&command, 1000U, &result) == ESP_OK);
    assert(result.applied);
    s_stop_result = ESP_ERR_NO_MEM;
    assert(home_ai_local_voice_command_execute(&command, 1100U, &result) == ESP_ERR_NO_MEM);
    assert(!result.applied);

    reset_fixture();
    for (size_t index = 0U; index < HOME_AI_MAX_USER_OVERRIDES; ++index) {
        home_ai_user_override_t override = {0};
        snprintf(override.override_id, sizeof(override.override_id), "existing_%zu", index);
        strcpy(override.room_id, "bedroom_02");
        override.action = HOME_AI_OVERRIDE_KEEP_ON;
        override.priority = 900U;
        override.created_at_ms = 1U;
        assert(home_ai_user_override_upsert(&override) == ESP_OK);
    }
    command = parse("静音", "bedroom_01");
    assert(home_ai_local_voice_command_execute(&command, 1200U, &result) == ESP_ERR_NO_MEM);
    assert(!result.applied);
    assert(s_mute_calls == 2U);
    assert(!s_muted);
    assert(!has_override("offline_mute_bedroom_01", HOME_AI_OVERRIDE_MUTE));
}

static void test_pause_keep_resume_and_undo(void)
{
    reset_fixture();
    home_ai_local_voice_command_result_t result = {0};
    home_ai_local_voice_command_t command = parse("暂停自动控制", "bedroom_01");
    assert(home_ai_local_voice_command_execute(&command, 2000U, &result) == ESP_OK);
    assert(has_override("offline_pause_bedroom_01", HOME_AI_OVERRIDE_PAUSE_AUTOMATION));

    command = parse("保持打开", "bedroom_01");
    assert(home_ai_local_voice_command_execute(&command, 2100U, &result) == ESP_OK);
    assert(has_override("offline_keep_bedroom_01", HOME_AI_OVERRIDE_KEEP_ON));
    home_ai_virtual_device_state_t state = {0};
    assert(home_ai_virtual_device_get("bedroom_01_light", &state));
    assert(state.power == HOME_AI_VIRTUAL_POWER_ON);

    command = parse("恢复自动控制", "bedroom_01");
    assert(home_ai_local_voice_command_execute(&command, 2200U, &result) == ESP_OK);
    assert(!has_override("offline_pause_bedroom_01", HOME_AI_OVERRIDE_PAUSE_AUTOMATION));
    assert(!has_override("offline_keep_bedroom_01", HOME_AI_OVERRIDE_KEEP_ON));

    command = parse("不要这样做", "bedroom_01");
    assert(home_ai_local_voice_command_execute(&command, 2300U, &result) == ESP_OK);
    assert(home_ai_virtual_device_get("bedroom_01_light", &state));
    assert(state.power == HOME_AI_VIRTUAL_POWER_OFF);
    assert(s_decision_events == 2U);
    assert(s_state_events == 2U);
    assert(s_override_events == 2U);
}

int main(void)
{
    test_fixed_vocabulary_and_room_selection();
    test_rejects_open_ended_and_ambiguous_text();
    test_stop_and_mute_failure_rollback();
    test_pause_keep_resume_and_undo();
    puts("home ai local voice command host tests: PASS");
    return 0;
}
