#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "home_ai_rule_engine.h"
#include "home_ai_user_override.h"

static const char *payload_v1 =
    "{"
    "\"schema_version\":1,\"version\":1,\"generated_at_ms\":1,"
    "\"rooms\":[{\"room_id\":\"bedroom_01\"}],"
    "\"rules\":["
    "{\"rule_id\":\"low_on\",\"version\":1,\"rule_type\":\"basic_automation\","
    "\"room_id\":\"bedroom_01\",\"enabled\":true,\"priority\":500,"
    "\"conditions\":[{\"field\":\"presence_state\",\"operator\":\"eq\",\"value\":\"occupied\",\"duration_ms\":1000}],"
    "\"actions\":[{\"device_id\":\"bedroom_light\",\"device_type\":\"light\",\"action\":\"turn_on\"}],"
    "\"cooldown_seconds\":2,\"offline_policy\":\"continue\"},"
    "{\"rule_id\":\"high_off\",\"version\":1,\"rule_type\":\"basic_automation\","
    "\"room_id\":\"bedroom_01\",\"enabled\":true,\"priority\":700,"
    "\"conditions\":[{\"field\":\"presence_state\",\"operator\":\"eq\",\"value\":\"occupied\",\"duration_ms\":1000}],"
    "\"actions\":[{\"device_id\":\"bedroom_light\",\"device_type\":\"light\",\"action\":\"turn_off\"}],"
    "\"cooldown_seconds\":2,\"offline_policy\":\"continue\"}"
    "]}";

static const char *payload_v2_partial =
    "{"
    "\"schema_version\":1,\"version\":2,\"generated_at_ms\":2,"
    "\"rooms\":[{\"room_id\":\"bedroom_01\"}],"
    "\"rules\":["
    "{\"rule_id\":\"high_off\",\"version\":2,\"rule_type\":\"basic_automation\","
    "\"room_id\":\"bedroom_01\",\"enabled\":true,\"priority\":700,"
    "\"conditions\":[{\"field\":\"presence_state\",\"operator\":\"eq\",\"value\":\"occupied\"}],"
    "\"actions\":[{\"device_id\":\"bedroom_light\",\"device_type\":\"light\",\"action\":\"set_color\"}],"
    "\"cooldown_seconds\":2,\"offline_policy\":\"continue\"},"
    "{\"rule_id\":\"valid_fan\",\"version\":1,\"rule_type\":\"basic_automation\","
    "\"room_id\":\"bedroom_01\",\"enabled\":true,\"priority\":600,"
    "\"conditions\":[{\"field\":\"stable_target_count\",\"operator\":\"gte\",\"value\":1}],"
    "\"actions\":[{\"device_id\":\"bedroom_fan\",\"device_type\":\"fan\",\"action\":\"turn_on\"}],"
    "\"cooldown_seconds\":2,\"offline_policy\":\"continue\"}"
    "]}";

static const char *payload_safety =
    "{"
    "\"schema_version\":1,\"version\":3,\"generated_at_ms\":3,"
    "\"rooms\":[{\"room_id\":\"bedroom_01\"}],"
    "\"rules\":["
    "{\"rule_id\":\"safety_off\",\"version\":1,\"rule_type\":\"safety\","
    "\"room_id\":\"bedroom_01\",\"enabled\":true,\"priority\":100,"
    "\"conditions\":[{\"field\":\"presence_state\",\"operator\":\"eq\",\"value\":\"occupied\"}],"
    "\"actions\":[{\"device_id\":\"bedroom_light\",\"device_type\":\"light\",\"action\":\"turn_off\"}],"
    "\"cooldown_seconds\":1,\"offline_policy\":\"continue\"}"
    "]}";

static const char *payload_weather =
    "{"
    "\"schema_version\":1,\"version\":6,\"generated_at_ms\":6,"
    "\"rooms\":[{\"room_id\":\"bedroom_01\"}],"
    "\"rules\":["
    "{\"rule_id\":\"weather_light\",\"version\":1,\"rule_type\":\"basic_automation\","
    "\"room_id\":\"bedroom_01\",\"enabled\":true,\"priority\":500,"
    "\"conditions\":[{\"field\":\"weather_dark\",\"operator\":\"eq\",\"value\":true}],"
    "\"actions\":[{\"device_id\":\"bedroom_light\",\"device_type\":\"light\",\"action\":\"turn_on\"}],"
    "\"cooldown_seconds\":1,\"offline_policy\":\"continue\"}"
    "]}";

static const char *payload_unknown_off =
    "{"
    "\"schema_version\":1,\"version\":7,\"generated_at_ms\":7,"
    "\"rooms\":[{\"room_id\":\"bedroom_01\"}],"
    "\"rules\":["
    "{\"rule_id\":\"unknown_off\",\"version\":1,\"rule_type\":\"basic_automation\","
    "\"room_id\":\"bedroom_01\",\"enabled\":true,\"priority\":500,"
    "\"conditions\":[{\"field\":\"radar_fresh\",\"operator\":\"eq\",\"value\":false}],"
    "\"actions\":[{\"device_id\":\"bedroom_light\",\"device_type\":\"light\",\"action\":\"turn_off\"}],"
    "\"cooldown_seconds\":1,\"offline_policy\":\"continue\"}"
    "]}";

static const char *payload_unknown_safety_off =
    "{"
    "\"schema_version\":1,\"version\":8,\"generated_at_ms\":8,"
    "\"rooms\":[{\"room_id\":\"bedroom_01\"}],"
    "\"rules\":["
    "{\"rule_id\":\"unknown_safety_off\",\"version\":1,\"rule_type\":\"safety\","
    "\"room_id\":\"bedroom_01\",\"enabled\":true,\"priority\":100,"
    "\"conditions\":[{\"field\":\"radar_fresh\",\"operator\":\"eq\",\"value\":false}],"
    "\"actions\":[{\"device_id\":\"bedroom_light\",\"device_type\":\"light\",\"action\":\"turn_off\"}],"
    "\"cooldown_seconds\":1,\"offline_policy\":\"continue\"}"
    "]}";

static home_ai_rule_evaluation_context_t context_at(uint64_t now_ms)
{
    static home_ai_room_state_t room;
    static home_ai_rule_environment_t environment;
    memset(&room, 0, sizeof(room));
    strcpy(room.room_id, "bedroom_01");
    room.presence_state = HOME_AI_ROOM_PRESENCE_OCCUPIED;
    room.stable_target_count = 1U;
    room.occupancy_mode = HOME_AI_ROOM_OCCUPANCY_SINGLE;
    room.radar_fresh = true;
    room.environment_fresh = true;
    environment.valid = true;
    environment.temperature_c = 25.0f;
    environment.humidity_percent = 45.0f;
    environment.air_quality_score = 20.0f;
    return (home_ai_rule_evaluation_context_t){
        .rooms = &room,
        .room_count = 1U,
        .environment = &environment,
        .environment_count = 1U,
        .time_window = "night",
        .server_online = true,
        .weather_available = true,
        .weather_dark = true,
        .now_ms = now_ms,
    };
}

static void test_duration_priority_and_cooldown(void)
{
    assert(home_ai_rule_engine_init());
    home_ai_rule_engine_reset();
    assert(home_ai_user_override_manager_init());
    home_ai_rule_activation_result_t activation = {0};
    assert(home_ai_rule_engine_apply_payload(payload_v1, strlen(payload_v1), &activation) == ESP_OK);
    assert(activation.state == HOME_AI_RULE_ACTIVATION_ACTIVE);
    assert(activation.active_rule_count == 2U);

    home_ai_rule_decision_t decisions[HOME_AI_MAX_PENDING_DECISIONS] = {0};
    home_ai_rule_evaluation_context_t context = context_at(100U);
    assert(home_ai_rule_engine_evaluate(&context, decisions, HOME_AI_MAX_PENDING_DECISIONS) == 0U);
    context.now_ms = 1099U;
    assert(home_ai_rule_engine_evaluate(&context, decisions, HOME_AI_MAX_PENDING_DECISIONS) == 0U);
    context.now_ms = 1100U;
    const size_t count = home_ai_rule_engine_evaluate(&context, decisions, HOME_AI_MAX_PENDING_DECISIONS);
    assert(count == 2U);
    assert(strcmp(decisions[0].rule_id, "high_off") == 0);
    assert(decisions[0].state == HOME_AI_RULE_DECISION_EXECUTE);
    assert(strcmp(decisions[1].rule_id, "low_on") == 0);
    assert(decisions[1].state == HOME_AI_RULE_DECISION_SUPPRESSED_PRIORITY);
    context.now_ms = 2500U;
    assert(home_ai_rule_engine_evaluate(&context, decisions, HOME_AI_MAX_PENDING_DECISIONS) == 0U);
    context.now_ms = 3100U;
    assert(home_ai_rule_engine_evaluate(&context, decisions, HOME_AI_MAX_PENDING_DECISIONS) == 2U);
}

static void test_override_safety_and_partial_activation(void)
{
    home_ai_user_override_t override = {0};
    strcpy(override.override_id, "keep_light_on");
    strcpy(override.room_id, "bedroom_01");
    strcpy(override.device_id, "bedroom_light");
    override.action = HOME_AI_OVERRIDE_KEEP_ON;
    override.priority = 900U;
    override.created_at_ms = 1U;
    override.expires_at_ms = 100000U;
    override.allow_safety_override = true;
    assert(home_ai_user_override_upsert(&override) == ESP_OK);

    home_ai_rule_activation_result_t activation = {0};
    assert(home_ai_rule_engine_apply_payload(payload_v2_partial,
                                             strlen(payload_v2_partial),
                                             &activation) == ESP_OK);
    assert(activation.state == HOME_AI_RULE_ACTIVATION_ACTIVE_PARTIAL);
    assert(activation.accepted_count == 1U);
    assert(activation.rejected_count == 1U);
    assert(activation.items[0].retained_previous);

    home_ai_rule_decision_t decisions[HOME_AI_MAX_PENDING_DECISIONS] = {0};
    home_ai_rule_evaluation_context_t context = context_at(2000U);
    assert(home_ai_rule_engine_evaluate(&context, decisions, HOME_AI_MAX_PENDING_DECISIONS) == 1U);
    assert(strcmp(decisions[0].rule_id, "valid_fan") == 0);
    assert(decisions[0].state == HOME_AI_RULE_DECISION_EXECUTE);
    context.now_ms = 3000U;
    assert(home_ai_rule_engine_evaluate(&context, decisions, HOME_AI_MAX_PENDING_DECISIONS) == 1U);
    assert(strcmp(decisions[0].rule_id, "high_off") == 0);
    assert(decisions[0].state == HOME_AI_RULE_DECISION_SUPPRESSED_OVERRIDE);

    assert(home_ai_rule_engine_apply_payload(payload_safety, strlen(payload_safety), &activation) == ESP_OK);
    context.now_ms = 4000U;
    assert(home_ai_rule_engine_evaluate(&context, decisions, HOME_AI_MAX_PENDING_DECISIONS) == 1U);
    assert(decisions[0].safety_action);
    assert(decisions[0].state == HOME_AI_RULE_DECISION_EXECUTE);
}

static void test_global_rejections_preserve_active_rules(void)
{
    const char *empty_package =
        "{\"schema_version\":1,\"version\":4,\"rooms\":[{\"room_id\":\"bedroom_01\"}],\"rules\":[]}";
    home_ai_rule_activation_result_t before = {0};
    home_ai_rule_activation_result_t after = {0};
    assert(home_ai_rule_engine_get_activation(&before));
    assert(home_ai_rule_engine_apply_payload(empty_package, strlen(empty_package), &after) == ESP_OK);
    assert(after.state == HOME_AI_RULE_ACTIVATION_ACTIVE);

    char oversized[4096] = {0};
    size_t written = (size_t)snprintf(oversized,
                                      sizeof(oversized),
                                      "{\"schema_version\":1,\"version\":5,\"rooms\":[{\"room_id\":\"bedroom_01\"}],\"rules\":[");
    for (size_t index = 0U; index < HOME_AI_MAX_RULES + 1U; ++index) {
        written += (size_t)snprintf(oversized + written,
                                    sizeof(oversized) - written,
                                    "%s{}",
                                    index == 0U ? "" : ",");
    }
    written += (size_t)snprintf(oversized + written,
                                sizeof(oversized) - written,
                                "]}");
    assert(written < sizeof(oversized));
    assert(home_ai_rule_engine_apply_payload(oversized, written, &after) == ESP_ERR_INVALID_ARG);
    assert(home_ai_rule_engine_get_activation(&after));
    assert(after.package_version == 4U);
    assert(after.active_rule_count == 0U);

    const char *duplicate =
        "{\"schema_version\":1,\"version\":5,\"rooms\":[{\"room_id\":\"bedroom_01\"}],\"rules\":["
        "{\"rule_id\":\"dup\"},{\"rule_id\":\"dup\"}]}";
    assert(home_ai_rule_engine_apply_payload(duplicate, strlen(duplicate), &after) == ESP_ERR_INVALID_ARG);
    assert(home_ai_rule_engine_get_activation(&after));
    assert(after.package_version == 4U);
    assert(after.active_rule_count == 0U);
    assert(before.package_version == 3U);
}

static void test_two_generation_restore_and_rollback(void)
{
    home_ai_rule_engine_reset();
    home_ai_rule_activation_result_t activation = {0};
    assert(home_ai_rule_engine_apply_payload(payload_v1, strlen(payload_v1), &activation) == ESP_OK);
    assert(activation.package_version == 1U);
    assert(home_ai_rule_engine_apply_payload(payload_v2_partial,
                                             strlen(payload_v2_partial),
                                             &activation) == ESP_OK);
    assert(activation.package_version == 2U);
    assert(home_ai_rule_engine_rollback(&activation));
    assert(activation.package_version == 1U);
    assert(activation.active_rule_count == 2U);
}

static void test_weather_rules_fail_closed(void)
{
    home_ai_rule_engine_reset();
    assert(home_ai_user_override_manager_init());
    home_ai_rule_activation_result_t activation = {0};
    assert(home_ai_rule_engine_apply_payload(payload_weather,
                                             strlen(payload_weather),
                                             &activation) == ESP_OK);
    home_ai_rule_decision_t decisions[HOME_AI_MAX_PENDING_DECISIONS] = {0};
    home_ai_rule_evaluation_context_t context = context_at(100U);
    context.weather_available = false;
    context.weather_dark = true;
    assert(home_ai_rule_engine_evaluate(&context,
                                        decisions,
                                        HOME_AI_MAX_PENDING_DECISIONS) == 0U);
    context.now_ms = 200U;
    context.weather_available = true;
    assert(home_ai_rule_engine_evaluate(&context,
                                        decisions,
                                        HOME_AI_MAX_PENDING_DECISIONS) == 1U);
    assert(strcmp(decisions[0].rule_id, "weather_light") == 0);
}

static void test_unknown_presence_suppresses_automatic_light_off(void)
{
    home_ai_rule_engine_reset();
    assert(home_ai_user_override_manager_init());
    home_ai_rule_activation_result_t activation = {0};
    assert(home_ai_rule_engine_apply_payload(payload_unknown_off,
                                             strlen(payload_unknown_off),
                                             &activation) == ESP_OK);
    home_ai_rule_decision_t decisions[HOME_AI_MAX_PENDING_DECISIONS] = {0};
    home_ai_rule_evaluation_context_t context = context_at(100U);
    ((home_ai_room_state_t *)context.rooms)->presence_state = HOME_AI_ROOM_PRESENCE_UNKNOWN;
    ((home_ai_room_state_t *)context.rooms)->radar_fresh = false;
    assert(home_ai_rule_engine_evaluate(&context,
                                        decisions,
                                        HOME_AI_MAX_PENDING_DECISIONS) == 1U);
    assert(decisions[0].state == HOME_AI_RULE_DECISION_SUPPRESSED_UNKNOWN_PRESENCE);
    assert(strcmp(home_ai_rule_decision_state_name(decisions[0].state),
                  "suppressed_unknown_presence") == 0);

    assert(home_ai_rule_engine_apply_payload(payload_unknown_safety_off,
                                             strlen(payload_unknown_safety_off),
                                             &activation) == ESP_OK);
    context.now_ms = 200U;
    assert(home_ai_rule_engine_evaluate(&context,
                                        decisions,
                                        HOME_AI_MAX_PENDING_DECISIONS) == 1U);
    assert(decisions[0].safety_action);
    assert(decisions[0].state == HOME_AI_RULE_DECISION_EXECUTE);
}

static home_ai_user_override_t override_for(const char *override_id,
                                            const char *device_id,
                                            home_ai_override_action_t action)
{
    home_ai_user_override_t value = {0};
    strcpy(value.override_id, override_id);
    strcpy(value.room_id, "bedroom_01");
    strcpy(value.device_id, device_id);
    value.action = action;
    value.priority = 900U;
    value.created_at_ms = 100U;
    value.expires_at_ms = 10000U;
    value.allow_safety_override = true;
    return value;
}

static void test_synced_override_snapshot_preserves_local_entries(void)
{
    assert(home_ai_user_override_manager_init());
    home_ai_user_override_t local = override_for("local_override", "bedroom_fan", HOME_AI_OVERRIDE_KEEP_ON);
    assert(home_ai_user_override_upsert(&local) == ESP_OK);

    home_ai_user_override_t synced = override_for("server_override", "bedroom_light", HOME_AI_OVERRIDE_KEEP_OFF);
    assert(home_ai_user_override_replace_synced(&synced, 1U, 1000U) == ESP_OK);
    home_ai_user_override_t snapshot[HOME_AI_MAX_USER_OVERRIDES] = {0};
    assert(home_ai_user_override_snapshot(snapshot, HOME_AI_MAX_USER_OVERRIDES) == 2U);

    home_ai_user_override_t replacement = override_for("server_replacement", "bedroom_ac", HOME_AI_OVERRIDE_KEEP_ON);
    assert(home_ai_user_override_replace_synced(&replacement, 1U, 2000U) == ESP_OK);
    const size_t count = home_ai_user_override_snapshot(snapshot, HOME_AI_MAX_USER_OVERRIDES);
    assert(count == 2U);
    bool saw_local = false;
    bool saw_replacement = false;
    for (size_t index = 0U; index < count; ++index) {
        saw_local = saw_local || strcmp(snapshot[index].override_id, "local_override") == 0;
        saw_replacement = saw_replacement || strcmp(snapshot[index].override_id, "server_replacement") == 0;
        assert(strcmp(snapshot[index].override_id, "server_override") != 0);
    }
    assert(saw_local && saw_replacement);

    home_ai_user_override_t collision = override_for("local_override", "bedroom_light", HOME_AI_OVERRIDE_KEEP_OFF);
    assert(home_ai_user_override_replace_synced(&collision, 1U, 3000U) == ESP_ERR_INVALID_STATE);
    assert(home_ai_user_override_snapshot(snapshot, HOME_AI_MAX_USER_OVERRIDES) == 2U);
}

static void test_reinitialization_clears_state(void)
{
    home_ai_rule_engine_reset();
    home_ai_rule_activation_result_t activation = {0};
    assert(home_ai_rule_engine_apply_payload(payload_v1, strlen(payload_v1), &activation) == ESP_OK);
    assert(activation.state == HOME_AI_RULE_ACTIVATION_ACTIVE);

    assert(home_ai_rule_engine_init());
    assert(home_ai_rule_engine_get_activation(&activation));
    assert(activation.state == HOME_AI_RULE_ACTIVATION_EMPTY);
    assert(activation.package_version == 0U);
    assert(activation.active_rule_count == 0U);

    assert(home_ai_rule_engine_apply_payload(payload_v1, strlen(payload_v1), &activation) == ESP_OK);
    assert(activation.state == HOME_AI_RULE_ACTIVATION_ACTIVE);
}

int main(void)
{
    test_duration_priority_and_cooldown();
    test_override_safety_and_partial_activation();
    test_global_rejections_preserve_active_rules();
    test_two_generation_restore_and_rollback();
    test_weather_rules_fail_closed();
    test_unknown_presence_suppresses_automatic_light_off();
    test_synced_override_snapshot_preserves_local_entries();
    test_reinitialization_clears_state();
    puts("home ai rule engine host tests: PASS");
    return 0;
}
