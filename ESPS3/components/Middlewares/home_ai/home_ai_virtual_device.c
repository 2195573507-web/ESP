#include "home_ai_virtual_device.h"

#include <string.h>

#ifndef HOME_AI_RULE_ENGINE_HOST_TEST
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#endif

static home_ai_virtual_device_state_t *s_devices;
static bool s_initialized;

#ifdef HOME_AI_RULE_ENGINE_HOST_TEST
static home_ai_virtual_device_state_t s_host_devices[HOME_AI_MAX_VIRTUAL_DEVICES];
#define VIRTUAL_DEVICE_LOCK() ((void)0)
#define VIRTUAL_DEVICE_UNLOCK() ((void)0)
#else
static portMUX_TYPE s_virtual_device_lock = portMUX_INITIALIZER_UNLOCKED;
#define VIRTUAL_DEVICE_LOCK() portENTER_CRITICAL(&s_virtual_device_lock)
#define VIRTUAL_DEVICE_UNLOCK() portEXIT_CRITICAL(&s_virtual_device_lock)
#endif

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

static bool valid_text(const char *text, size_t max_len)
{
    if (text == NULL || text[0] == '\0') return false;
    for (size_t index = 0U; index < max_len; ++index) {
        if (text[index] == '\0') return true;
    }
    return false;
}

static const char *virtual_action_name(home_ai_rule_action_t action)
{
    switch (action) {
    case HOME_AI_RULE_ACTION_TURN_ON: return "turn_on";
    case HOME_AI_RULE_ACTION_TURN_OFF: return "turn_off";
    case HOME_AI_RULE_ACTION_PAUSE_AUTOMATION: return "pause_automation";
    case HOME_AI_RULE_ACTION_RESUME_AUTOMATION: return "resume_automation";
    case HOME_AI_RULE_ACTION_PLAY_PROMPT: return "play_prompt";
    default: return "unknown";
    }
}

static home_ai_virtual_device_state_t *find_device_locked(const char *device_id)
{
    for (size_t index = 0U; index < HOME_AI_MAX_VIRTUAL_DEVICES; ++index) {
        if (s_devices[index].valid && strcmp(s_devices[index].device_id, device_id) == 0) {
            return &s_devices[index];
        }
    }
    return NULL;
}

static home_ai_virtual_device_state_t *allocate_device_locked(const home_ai_rule_decision_t *decision)
{
    for (size_t index = 0U; index < HOME_AI_MAX_VIRTUAL_DEVICES; ++index) {
        if (!s_devices[index].valid) {
            home_ai_virtual_device_state_t *device = &s_devices[index];
            memset(device, 0, sizeof(*device));
            device->valid = true;
            copy_text(device->device_id, sizeof(device->device_id), decision->device_id);
            copy_text(device->room_id, sizeof(device->room_id), decision->room_id);
            device->device_type = decision->device_type;
            device->power = HOME_AI_VIRTUAL_POWER_OFF;
            copy_text(device->last_action, sizeof(device->last_action), "turn_off");
            copy_text(device->action_source, sizeof(device->action_source), "virtual_initial_state");
            copy_text(device->decision_reason,
                      sizeof(device->decision_reason),
                      "initial_virtual_state");
            device->verified = true;
            return device;
        }
    }
    return NULL;
}

bool home_ai_virtual_device_executor_init(void)
{
#ifdef HOME_AI_RULE_ENGINE_HOST_TEST
    s_devices = s_host_devices;
#else
    if (s_devices == NULL) {
        s_devices = heap_caps_calloc(HOME_AI_MAX_VIRTUAL_DEVICES,
                                     sizeof(*s_devices),
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_devices == NULL) return false;
    }
#endif
    VIRTUAL_DEVICE_LOCK();
    memset(s_devices, 0, HOME_AI_MAX_VIRTUAL_DEVICES * sizeof(*s_devices));
    s_initialized = true;
    VIRTUAL_DEVICE_UNLOCK();
    return true;
}

esp_err_t home_ai_virtual_device_execute(const home_ai_rule_decision_t *decision,
                                         uint64_t now_ms,
                                         bool explicit_user_command,
                                         home_ai_virtual_device_execution_t *out)
{
    if (out != NULL) memset(out, 0, sizeof(*out));
    if (!s_initialized || decision == NULL || now_ms == 0U ||
        (decision->action != HOME_AI_RULE_ACTION_TURN_ON &&
         decision->action != HOME_AI_RULE_ACTION_TURN_OFF) ||
        !valid_text(decision->device_id, sizeof(decision->device_id)) ||
        !valid_text(decision->room_id, sizeof(decision->room_id))) {
        return ESP_ERR_INVALID_ARG;
    }
    VIRTUAL_DEVICE_LOCK();
    home_ai_virtual_device_state_t *device = find_device_locked(decision->device_id);
    if (device == NULL) device = allocate_device_locked(decision);
    if (device == NULL) {
        VIRTUAL_DEVICE_UNLOCK();
        return ESP_ERR_NO_MEM;
    }
    if (device->device_type != decision->device_type || strcmp(device->room_id, decision->room_id) != 0) {
        VIRTUAL_DEVICE_UNLOCK();
        return ESP_ERR_INVALID_STATE;
    }
    const home_ai_virtual_power_t target = decision->action == HOME_AI_RULE_ACTION_TURN_ON ?
                                              HOME_AI_VIRTUAL_POWER_ON : HOME_AI_VIRTUAL_POWER_OFF;
    home_ai_virtual_execution_result_t execution = HOME_AI_VIRTUAL_EXECUTION_APPLIED;
    const uint64_t hold_ms = (uint64_t)decision->minimum_active_seconds * 1000U;
    if (device->power == target) {
        execution = HOME_AI_VIRTUAL_EXECUTION_NOOP;
        copy_text(device->last_action, sizeof(device->last_action), virtual_action_name(decision->action));
        copy_text(device->action_source,
                  sizeof(device->action_source),
                  explicit_user_command ? "explicit_user_command" :
                  decision->safety_action ? "safety_rule" : "automation_rule");
        copy_text(device->decision_id, sizeof(device->decision_id), decision->decision_id);
        copy_text(device->decision_reason, sizeof(device->decision_reason), decision->rule_id);
        device->verified = true;
        device->updated_at_ms = now_ms;
    } else if (!explicit_user_command && !decision->safety_action &&
               device->minimum_transition_at_ms != 0U && now_ms < device->minimum_transition_at_ms) {
        execution = HOME_AI_VIRTUAL_EXECUTION_DEFERRED_MINIMUM;
    } else {
        device->power = target;
        device->minimum_transition_at_ms = hold_ms == 0U ? 0U : now_ms + hold_ms;
        copy_text(device->last_action, sizeof(device->last_action), virtual_action_name(decision->action));
        copy_text(device->action_source,
                  sizeof(device->action_source),
                  explicit_user_command ? "explicit_user_command" :
                  decision->safety_action ? "safety_rule" : "automation_rule");
        copy_text(device->decision_id, sizeof(device->decision_id), decision->decision_id);
        copy_text(device->decision_reason, sizeof(device->decision_reason), decision->rule_id);
        device->verified = true;
        device->updated_at_ms = now_ms;
    }
    if (out != NULL) {
        out->result = execution;
        out->state = *device;
        copy_text(out->reason,
                  sizeof(out->reason),
                  execution == HOME_AI_VIRTUAL_EXECUTION_APPLIED ? "virtual_state_written" :
                  execution == HOME_AI_VIRTUAL_EXECUTION_NOOP ? "already_in_requested_state" :
                  "minimum_transition_active");
    }
    VIRTUAL_DEVICE_UNLOCK();
    return ESP_OK;
}

bool home_ai_virtual_device_get(const char *device_id, home_ai_virtual_device_state_t *out)
{
    if (!s_initialized || device_id == NULL || device_id[0] == '\0' || out == NULL) return false;
    VIRTUAL_DEVICE_LOCK();
    home_ai_virtual_device_state_t *device = find_device_locked(device_id);
    if (device != NULL) *out = *device;
    VIRTUAL_DEVICE_UNLOCK();
    return device != NULL;
}

size_t home_ai_virtual_device_snapshot(home_ai_virtual_device_state_t *out, size_t capacity)
{
    if (!s_initialized || out == NULL || capacity == 0U) return 0U;
    size_t copied = 0U;
    VIRTUAL_DEVICE_LOCK();
    for (size_t index = 0U; index < HOME_AI_MAX_VIRTUAL_DEVICES && copied < capacity; ++index) {
        if (s_devices[index].valid) out[copied++] = s_devices[index];
    }
    VIRTUAL_DEVICE_UNLOCK();
    return copied;
}

const char *home_ai_virtual_power_name(home_ai_virtual_power_t power)
{
    return power == HOME_AI_VIRTUAL_POWER_ON ? "on" : "off";
}

const char *home_ai_virtual_execution_result_name(home_ai_virtual_execution_result_t result)
{
    switch (result) {
    case HOME_AI_VIRTUAL_EXECUTION_APPLIED: return "applied";
    case HOME_AI_VIRTUAL_EXECUTION_NOOP: return "noop";
    case HOME_AI_VIRTUAL_EXECUTION_DEFERRED_MINIMUM: return "deferred_minimum";
    case HOME_AI_VIRTUAL_EXECUTION_REJECTED: return "rejected";
    default: return "unknown";
    }
}
