#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "gateway_config.h"
#include "gateway_event_reporter.h"
#include "home_ai_event_reporter.h"
#include "home_ai_user_override.h"
#include "home_ai_virtual_device.h"
#include "offline_policy.h"
#include "server_client.h"
#include "sensor_aggregator.h"
#include "smart_home_gateway.h"

static const char *s_pending_body;
static esp_err_t s_execute_ret;
static home_ai_virtual_device_execution_t s_execution_template;
static esp_err_t s_ack_ret;
static int s_ack_status;
static char s_last_ack[4096];
static int s_override_calls;
static bool s_last_completed;
static int s_override_event_calls;

static const gateway_runtime_config_t s_config = {
    .gateway_id = "sensair_s3_gateway_01",
};

int64_t esp_timer_get_time(void)
{
    return 1234000LL;
}

const char *esp_err_to_name(esp_err_t code)
{
    return code == ESP_OK ? "ESP_OK" : "ESP_FAIL";
}

const gateway_runtime_config_t *gateway_config_get(void)
{
    return &s_config;
}

esp_err_t home_ai_user_override_upsert(const home_ai_user_override_t *override)
{
    assert(override != NULL);
    ++s_override_calls;
    return ESP_OK;
}

void home_ai_event_reporter_record_user_override(const home_ai_user_override_t *override,
                                                 const char *device_id,
                                                 uint64_t occurred_at_ms)
{
    assert(override != NULL);
    assert(device_id != NULL);
    (void)occurred_at_ms;
    ++s_override_event_calls;
}

void home_ai_event_reporter_record_decision(const home_ai_rule_decision_t *decision,
                                            const home_ai_virtual_device_execution_t *execution,
                                            uint64_t occurred_at_ms)
{
    assert(decision != NULL);
    assert(execution != NULL);
    (void)occurred_at_ms;
}

void home_ai_event_reporter_record_virtual_state(const home_ai_virtual_device_state_t *state,
                                                 uint64_t occurred_at_ms)
{
    assert(state != NULL);
    (void)occurred_at_ms;
}

esp_err_t home_ai_virtual_device_execute(const home_ai_rule_decision_t *decision,
                                         uint64_t now_ms,
                                         bool explicit_user_command,
                                         home_ai_virtual_device_execution_t *out)
{
    assert(decision != NULL);
    assert(explicit_user_command);
    assert(now_ms > 0U);
    if (out != NULL) {
        *out = s_execution_template;
    }
    return s_execute_ret;
}

const char *home_ai_rule_device_type_name(home_ai_rule_device_type_t type)
{
    switch (type) {
    case HOME_AI_RULE_DEVICE_LIGHT: return "light";
    case HOME_AI_RULE_DEVICE_AIR_CONDITIONER: return "air_conditioner";
    case HOME_AI_RULE_DEVICE_FAN: return "fan";
    default: return "unknown";
    }
}

const char *home_ai_virtual_power_name(home_ai_virtual_power_t power)
{
    return power == HOME_AI_VIRTUAL_POWER_ON ? "on" : "off";
}

esp_err_t server_client_get_smart_home_pending(char *response_body,
                                               size_t response_body_size,
                                               int *http_status)
{
    assert(response_body != NULL);
    assert(s_pending_body != NULL);
    assert(strlen(s_pending_body) + 1U <= response_body_size);
    strcpy(response_body, s_pending_body);
    if (http_status != NULL) *http_status = 200;
    return ESP_OK;
}

esp_err_t server_client_ack_smart_home_command(const char *command_id,
                                               const char *ack_json,
                                               char *response_body,
                                               size_t response_body_size,
                                               int *http_status)
{
    assert(command_id != NULL);
    assert(ack_json != NULL);
    assert(strlen(ack_json) + 1U <= sizeof(s_last_ack));
    strcpy(s_last_ack, ack_json);
    if (response_body != NULL && response_body_size > 0U) response_body[0] = '\0';
    if (http_status != NULL) *http_status = s_ack_status;
    return s_ack_ret;
}

void offline_policy_record_server_result(esp_err_t ret, int http_status)
{
    (void)ret;
    (void)http_status;
}

void sensor_aggregator_record_command_ack(const char *device_id,
                                          const char *command_id,
                                          unsigned int command_code,
                                          bool completed)
{
    assert(device_id != NULL);
    assert(command_id != NULL);
    (void)command_code;
    s_last_completed = completed;
}

esp_err_t gateway_event_reporter_system(const char *device_id,
                                        const char *level,
                                        const char *message,
                                        const char *reason)
{
    (void)device_id;
    (void)level;
    (void)message;
    (void)reason;
    return ESP_OK;
}

esp_err_t gateway_event_reporter_alarm(const char *device_id,
                                       const char *level,
                                       const char *title,
                                       const char *message,
                                       const char *reason)
{
    (void)device_id;
    (void)level;
    (void)title;
    (void)message;
    (void)reason;
    return ESP_OK;
}

void gateway_event_reporter_record_server_state(bool available)
{
    (void)available;
}

static void reset_case(const char *command_id,
                       home_ai_virtual_execution_result_t execution_result,
                       bool state_valid,
                       bool state_verified,
                       esp_err_t execute_ret,
                       esp_err_t ack_ret,
                       int ack_status)
{
    static char body[512];
    (void)snprintf(body,
                   sizeof(body),
                   "{\"commands\":[{\"command_id\":\"%s\",\"target_id\":\"bedroom_light\","
                   "\"device_type\":\"light\",\"action\":\"turn_on\",\"room_id\":\"bedroom_01\"}]}",
                   command_id);
    s_pending_body = body;
    s_execute_ret = execute_ret;
    memset(&s_execution_template, 0, sizeof(s_execution_template));
    s_execution_template.result = execution_result;
    s_execution_template.state.valid = state_valid;
    s_execution_template.state.verified = state_verified;
    s_execution_template.state.power = HOME_AI_VIRTUAL_POWER_ON;
    strcpy(s_execution_template.reason, execute_ret == ESP_OK ? "virtual_state_written" : "virtual_execution_failed");
    s_ack_ret = ack_ret;
    s_ack_status = ack_status;
    s_last_ack[0] = '\0';
    s_override_calls = 0;
    s_override_event_calls = 0;
    s_last_completed = false;
}

static void test_failed_execution_does_not_create_override(void)
{
    reset_case("failed_command",
                HOME_AI_VIRTUAL_EXECUTION_REJECTED,
                false,
                false,
                ESP_ERR_INVALID_STATE,
                ESP_OK,
                200);
    smart_home_gateway_poll_once();
    assert(s_override_calls == 0);
    assert(s_override_event_calls == 0);
    assert(strstr(s_last_ack, "\"verified\":false") != NULL);
    assert(!s_last_completed);
}

static void test_local_success_stays_verified_when_ack_transport_fails(void)
{
    reset_case("transport_failed_command",
                HOME_AI_VIRTUAL_EXECUTION_APPLIED,
                true,
                true,
                ESP_OK,
                ESP_FAIL,
                503);
    smart_home_gateway_poll_once();
    assert(s_override_calls == 1);
    assert(s_override_event_calls == 1);
    assert(strstr(s_last_ack, "\"verified\":true") != NULL);
    assert(s_last_completed);
}

static void test_noop_success_creates_override(void)
{
    reset_case("noop_command",
                HOME_AI_VIRTUAL_EXECUTION_NOOP,
                true,
                true,
                ESP_OK,
                ESP_OK,
                200);
    smart_home_gateway_poll_once();
    assert(s_override_calls == 1);
    assert(strstr(s_last_ack, "\"status\":\"succeeded\"") != NULL);
    assert(strstr(s_last_ack, "\"verified\":true") != NULL);
    assert(s_last_completed);
}

int main(void)
{
    test_failed_execution_does_not_create_override();
    test_local_success_stays_verified_when_ack_transport_fails();
    test_noop_success_creates_override();
    puts("smart home gateway host tests: PASS");
    return 0;
}
