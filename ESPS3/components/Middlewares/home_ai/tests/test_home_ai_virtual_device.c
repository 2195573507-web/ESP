#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "home_ai_virtual_device.h"

static home_ai_rule_decision_t decision_for(const char *device_id,
                                            home_ai_rule_action_t action,
                                            home_ai_rule_device_type_t type,
                                            uint32_t minimum_seconds)
{
    home_ai_rule_decision_t decision = {0};
    strcpy(decision.decision_id, "decision_1");
    strcpy(decision.rule_id, "room_automation");
    strcpy(decision.room_id, "bedroom_01");
    strcpy(decision.device_id, device_id);
    decision.action = action;
    decision.device_type = type;
    decision.minimum_active_seconds = minimum_seconds;
    return decision;
}

static void test_virtual_state_and_minimum_transition(void)
{
    assert(home_ai_virtual_device_executor_init());
    home_ai_rule_decision_t on = decision_for("bedroom_light",
                                              HOME_AI_RULE_ACTION_TURN_ON,
                                              HOME_AI_RULE_DEVICE_LIGHT,
                                              60U);
    home_ai_virtual_device_execution_t execution = {0};
    assert(home_ai_virtual_device_execute(&on, 1000U, false, &execution) == ESP_OK);
    assert(execution.result == HOME_AI_VIRTUAL_EXECUTION_APPLIED);
    assert(execution.state.power == HOME_AI_VIRTUAL_POWER_ON);
    assert(execution.state.verified);

    home_ai_rule_decision_t off = decision_for("bedroom_light",
                                               HOME_AI_RULE_ACTION_TURN_OFF,
                                               HOME_AI_RULE_DEVICE_LIGHT,
                                               60U);
    assert(home_ai_virtual_device_execute(&off, 2000U, false, &execution) == ESP_OK);
    assert(execution.result == HOME_AI_VIRTUAL_EXECUTION_DEFERRED_MINIMUM);
    assert(execution.state.power == HOME_AI_VIRTUAL_POWER_ON);
    assert(home_ai_virtual_device_execute(&off, 2000U, true, &execution) == ESP_OK);
    assert(execution.result == HOME_AI_VIRTUAL_EXECUTION_APPLIED);
    assert(execution.state.power == HOME_AI_VIRTUAL_POWER_OFF);
}

static void test_fixed_capacity_and_type_binding(void)
{
    assert(home_ai_virtual_device_executor_init());
    home_ai_virtual_device_execution_t execution = {0};
    for (unsigned int index = 0U; index < HOME_AI_MAX_VIRTUAL_DEVICES; ++index) {
        char device_id[HOME_AI_RULE_DEVICE_ID_LEN];
        snprintf(device_id, sizeof(device_id), "light_%u", index);
        home_ai_rule_decision_t on = decision_for(device_id,
                                                  HOME_AI_RULE_ACTION_TURN_ON,
                                                  HOME_AI_RULE_DEVICE_LIGHT,
                                                  0U);
        assert(home_ai_virtual_device_execute(&on, 1000U + index, false, &execution) == ESP_OK);
    }
    home_ai_rule_decision_t overflow = decision_for("overflow_light",
                                                    HOME_AI_RULE_ACTION_TURN_ON,
                                                    HOME_AI_RULE_DEVICE_LIGHT,
                                                    0U);
    assert(home_ai_virtual_device_execute(&overflow, 2000U, false, &execution) == ESP_ERR_NO_MEM);

    assert(home_ai_virtual_device_executor_init());
    home_ai_rule_decision_t light = decision_for("shared_device",
                                                 HOME_AI_RULE_ACTION_TURN_ON,
                                                 HOME_AI_RULE_DEVICE_LIGHT,
                                                 0U);
    assert(home_ai_virtual_device_execute(&light, 1000U, false, &execution) == ESP_OK);
    home_ai_rule_decision_t fan = decision_for("shared_device",
                                               HOME_AI_RULE_ACTION_TURN_ON,
                                               HOME_AI_RULE_DEVICE_FAN,
                                               0U);
    assert(home_ai_virtual_device_execute(&fan, 2000U, false, &execution) == ESP_ERR_INVALID_STATE);
}

int main(void)
{
    test_virtual_state_and_minimum_transition();
    test_fixed_capacity_and_type_binding();
    puts("home ai virtual device host tests: PASS");
    return 0;
}
