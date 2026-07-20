#include "home_ai_user_override.h"

#include <string.h>

#ifndef HOME_AI_RULE_ENGINE_HOST_TEST
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#endif

typedef struct {
    bool active;
    bool synced;
    home_ai_user_override_t value;
} home_ai_override_slot_t;

static home_ai_override_slot_t *s_overrides;
static bool s_initialized;

#ifdef HOME_AI_RULE_ENGINE_HOST_TEST
static home_ai_override_slot_t s_host_overrides[HOME_AI_MAX_USER_OVERRIDES];
#define OVERRIDE_LOCK() ((void)0)
#define OVERRIDE_UNLOCK() ((void)0)
#else
static portMUX_TYPE s_override_lock = portMUX_INITIALIZER_UNLOCKED;
#define OVERRIDE_LOCK() portENTER_CRITICAL(&s_override_lock)
#define OVERRIDE_UNLOCK() portEXIT_CRITICAL(&s_override_lock)
#endif

static bool text_present(const char *text, size_t capacity)
{
    if (text == NULL || text[0] == '\0') {
        return false;
    }
    for (size_t index = 0U; index < capacity; ++index) {
        if (text[index] == '\0') {
            return true;
        }
    }
    return false;
}

static bool text_matches_scope(const char *configured, const char *value)
{
    return configured[0] == '\0' || (value != NULL && strcmp(configured, value) == 0);
}

static bool override_valid(const home_ai_user_override_t *override)
{
    return override != NULL &&
           text_present(override->override_id, sizeof(override->override_id)) &&
           (text_present(override->room_id, sizeof(override->room_id)) ||
            text_present(override->device_id, sizeof(override->device_id))) &&
           override->action <= HOME_AI_OVERRIDE_MUTE &&
           override->priority >= 800U && override->priority <= 999U;
}

static bool duplicate_id(const home_ai_user_override_t *overrides,
                         size_t current)
{
    for (size_t index = 0U; index < current; ++index) {
        if (strcmp(overrides[index].override_id, overrides[current].override_id) == 0) {
            return true;
        }
    }
    return false;
}

static bool expired(const home_ai_user_override_t *override, uint64_t now_ms)
{
    return override->expires_at_ms != 0U && now_ms >= override->expires_at_ms;
}

bool home_ai_user_override_manager_init(void)
{
#ifdef HOME_AI_RULE_ENGINE_HOST_TEST
    s_overrides = s_host_overrides;
#else
    if (s_overrides == NULL) {
        s_overrides = heap_caps_calloc(HOME_AI_MAX_USER_OVERRIDES,
                                       sizeof(*s_overrides),
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_overrides == NULL) return false;
    }
#endif
    OVERRIDE_LOCK();
    memset(s_overrides, 0, HOME_AI_MAX_USER_OVERRIDES * sizeof(*s_overrides));
    s_initialized = true;
    OVERRIDE_UNLOCK();
    return true;
}

esp_err_t home_ai_user_override_upsert(const home_ai_user_override_t *override)
{
    if (!override_valid(override)) {
        return ESP_ERR_INVALID_ARG;
    }
    OVERRIDE_LOCK();
    if (!s_initialized) {
        OVERRIDE_UNLOCK();
        return ESP_ERR_INVALID_STATE;
    }
    home_ai_override_slot_t *available = NULL;
    for (size_t index = 0U; index < HOME_AI_MAX_USER_OVERRIDES; ++index) {
        home_ai_override_slot_t *slot = &s_overrides[index];
        if (slot->active && strcmp(slot->value.override_id, override->override_id) == 0) {
            slot->value = *override;
            slot->synced = false;
            OVERRIDE_UNLOCK();
            return ESP_OK;
        }
        if (!slot->active && available == NULL) {
            available = slot;
        }
    }
    if (available == NULL) {
        OVERRIDE_UNLOCK();
        return ESP_ERR_NO_MEM;
    }
    available->value = *override;
    available->active = true;
    available->synced = false;
    OVERRIDE_UNLOCK();
    return ESP_OK;
}

esp_err_t home_ai_user_override_replace_synced(const home_ai_user_override_t *overrides,
                                               size_t count,
                                               uint64_t now_ms)
{
    if ((overrides == NULL && count != 0U) || count > HOME_AI_MAX_USER_OVERRIDES || now_ms == 0U) {
        return ESP_ERR_INVALID_ARG;
    }
    for (size_t index = 0U; index < count; ++index) {
        if (!override_valid(&overrides[index]) || duplicate_id(overrides, index)) {
            return ESP_ERR_INVALID_ARG;
        }
        if (expired(&overrides[index], now_ms)) {
            return ESP_ERR_INVALID_ARG;
        }
    }

    OVERRIDE_LOCK();
    if (!s_initialized) {
        OVERRIDE_UNLOCK();
        return ESP_ERR_INVALID_STATE;
    }
    size_t local_count = 0U;
    for (size_t index = 0U; index < HOME_AI_MAX_USER_OVERRIDES; ++index) {
        home_ai_override_slot_t *slot = &s_overrides[index];
        if (!slot->active) continue;
        if (expired(&slot->value, now_ms)) {
            memset(slot, 0, sizeof(*slot));
            continue;
        }
        if (!slot->synced) ++local_count;
        for (size_t incoming = 0U; incoming < count; ++incoming) {
            if (strcmp(slot->value.override_id, overrides[incoming].override_id) == 0 &&
                !slot->synced) {
                OVERRIDE_UNLOCK();
                return ESP_ERR_INVALID_STATE;
            }
        }
    }
    if (local_count + count > HOME_AI_MAX_USER_OVERRIDES) {
        OVERRIDE_UNLOCK();
        return ESP_ERR_NO_MEM;
    }
    for (size_t index = 0U; index < HOME_AI_MAX_USER_OVERRIDES; ++index) {
        if (s_overrides[index].active && s_overrides[index].synced) {
            memset(&s_overrides[index], 0, sizeof(s_overrides[index]));
        }
    }
    for (size_t incoming = 0U; incoming < count; ++incoming) {
        home_ai_override_slot_t *available = NULL;
        for (size_t index = 0U; index < HOME_AI_MAX_USER_OVERRIDES; ++index) {
            if (!s_overrides[index].active) {
                available = &s_overrides[index];
                break;
            }
        }
        if (available == NULL) {
            OVERRIDE_UNLOCK();
            return ESP_ERR_NO_MEM;
        }
        available->value = overrides[incoming];
        available->active = true;
        available->synced = true;
    }
    OVERRIDE_UNLOCK();
    return ESP_OK;
}

bool home_ai_user_override_remove(const char *override_id)
{
    if (override_id == NULL || override_id[0] == '\0') {
        return false;
    }
    OVERRIDE_LOCK();
    for (size_t index = 0U; index < HOME_AI_MAX_USER_OVERRIDES; ++index) {
        if (s_overrides[index].active &&
            strcmp(s_overrides[index].value.override_id, override_id) == 0) {
            memset(&s_overrides[index], 0, sizeof(s_overrides[index]));
            OVERRIDE_UNLOCK();
            return true;
        }
    }
    OVERRIDE_UNLOCK();
    return false;
}

void home_ai_user_override_expire(uint64_t now_ms)
{
    if (!s_initialized || now_ms == 0U) {
        return;
    }
    OVERRIDE_LOCK();
    for (size_t index = 0U; index < HOME_AI_MAX_USER_OVERRIDES; ++index) {
        if (s_overrides[index].active && expired(&s_overrides[index].value, now_ms)) {
            memset(&s_overrides[index], 0, sizeof(s_overrides[index]));
        }
    }
    OVERRIDE_UNLOCK();
}

size_t home_ai_user_override_snapshot(home_ai_user_override_t *out, size_t capacity)
{
    if (out == NULL || capacity == 0U || !s_initialized) {
        return 0U;
    }
    size_t copied = 0U;
    OVERRIDE_LOCK();
    for (size_t index = 0U; index < HOME_AI_MAX_USER_OVERRIDES && copied < capacity; ++index) {
        if (s_overrides[index].active) {
            out[copied++] = s_overrides[index].value;
        }
    }
    OVERRIDE_UNLOCK();
    return copied;
}

home_ai_override_decision_t home_ai_user_override_evaluate(const char *room_id,
                                                            const char *device_id,
                                                            const char *action,
                                                            bool safety_action,
                                                            uint64_t now_ms,
                                                            home_ai_user_override_t *out_match)
{
    if (!s_initialized || action == NULL || action[0] == '\0') {
        return HOME_AI_OVERRIDE_DECISION_NONE;
    }
    home_ai_override_slot_t *best = NULL;
    OVERRIDE_LOCK();
    for (size_t index = 0U; index < HOME_AI_MAX_USER_OVERRIDES; ++index) {
        home_ai_override_slot_t *slot = &s_overrides[index];
        if (!slot->active) {
            continue;
        }
        if (expired(&slot->value, now_ms)) {
            memset(slot, 0, sizeof(*slot));
            continue;
        }
        if (safety_action && slot->value.allow_safety_override) {
            continue;
        }
        if (!text_matches_scope(slot->value.room_id, room_id) ||
            !text_matches_scope(slot->value.device_id, device_id)) {
            continue;
        }
        if (best == NULL || slot->value.priority > best->value.priority ||
            (slot->value.priority == best->value.priority &&
             slot->value.created_at_ms > best->value.created_at_ms)) {
            best = slot;
        }
    }
    if (best != NULL && out_match != NULL) {
        *out_match = best->value;
    }
    home_ai_override_decision_t result = HOME_AI_OVERRIDE_DECISION_NONE;
    if (best != NULL) {
        if (best->value.action == HOME_AI_OVERRIDE_MUTE) {
            result = HOME_AI_OVERRIDE_DECISION_MUTE;
        } else if (best->value.action == HOME_AI_OVERRIDE_PAUSE_AUTOMATION ||
                   (best->value.action == HOME_AI_OVERRIDE_KEEP_ON &&
                    strcmp(action, "turn_off") == 0) ||
                   (best->value.action == HOME_AI_OVERRIDE_KEEP_OFF &&
                    strcmp(action, "turn_on") == 0)) {
            result = HOME_AI_OVERRIDE_DECISION_SUPPRESS;
        }
    }
    OVERRIDE_UNLOCK();
    return result;
}

const char *home_ai_override_action_name(home_ai_override_action_t action)
{
    switch (action) {
    case HOME_AI_OVERRIDE_KEEP_ON:
        return "keep_on";
    case HOME_AI_OVERRIDE_KEEP_OFF:
        return "keep_off";
    case HOME_AI_OVERRIDE_PAUSE_AUTOMATION:
        return "pause_automation";
    case HOME_AI_OVERRIDE_MUTE:
        return "mute";
    default:
        return "unknown";
    }
}
