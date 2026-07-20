#include "home_ai_emergency_coordinator.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#ifndef HOME_AI_EMERGENCY_HOST_TEST
#include "esp_heap_caps.h"
#endif
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "environment_alarm_reporter.h"
#include "home_ai_event_reporter.h"
#include "home_ai_voice_router.h"

#define HOME_AI_EMERGENCY_INITIAL_REPEAT_MS 60000U
#define HOME_AI_EMERGENCY_SECOND_REPEAT_MS 180000U
#define HOME_AI_EMERGENCY_THIRD_REPEAT_MS 300000U
#define HOME_AI_EMERGENCY_REPEATED_REPEAT_MS 300000U
#define HOME_AI_EMERGENCY_ACK_REPEAT_MS 600000U
#define HOME_AI_EMERGENCY_RETRY_MS 30000U
#define HOME_AI_EMERGENCY_RECOVERY_RETRY_MS 60000U
#define HOME_AI_EMERGENCY_RESOLVED_HOLD_MS 60000U
#define HOME_AI_EMERGENCY_MAX_DISPATCH_PER_TICK 2U

static const char *TAG = "home_ai_emergency";

typedef struct {
    bool used;
    bool dispatching;
    uint64_t alarm_id;
    uint8_t alarm_level;
    home_ai_emergency_state_t state;
    bool user_acknowledged;
    uint32_t playback_generation;
    uint64_t activated_at_ms;
    uint64_t schedule_base_ms;
    uint64_t next_prompt_at_ms;
    uint64_t resolved_at_ms;
    uint8_t repeat_stage;
    char event_id[HOME_AI_EMERGENCY_EVENT_ID_LEN];
    char room_id[32];
} home_ai_emergency_slot_t;

#ifdef HOME_AI_EMERGENCY_HOST_TEST
static home_ai_emergency_slot_t s_host_slots[HOME_AI_EMERGENCY_MAX_ACTIVE];
#else
static home_ai_emergency_slot_t *s_slots;
#endif
static home_ai_emergency_acknowledgement_t s_acknowledgements[HOME_AI_EMERGENCY_ACKNOWLEDGEMENT_MAX];
static StaticSemaphore_t s_lock_storage;
static SemaphoreHandle_t s_lock;
static bool s_initialized;

static home_ai_emergency_slot_t *slots(void)
{
#ifdef HOME_AI_EMERGENCY_HOST_TEST
    return s_host_slots;
#else
    return s_slots;
#endif
}

static uint64_t monotonic_now_ms(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000);
}

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

static bool valid_event_id(const char *value)
{
    if (value == NULL || value[0] == '\0') return false;
    for (size_t index = 0U; index < HOME_AI_EMERGENCY_EVENT_ID_LEN; ++index) {
        if (value[index] == '\0') return true;
    }
    return false;
}

static home_ai_emergency_slot_t *find_alarm_locked(uint64_t alarm_id)
{
    home_ai_emergency_slot_t *items = slots();
    for (size_t index = 0U; index < HOME_AI_EMERGENCY_MAX_ACTIVE; ++index) {
        if (items[index].used && items[index].alarm_id == alarm_id) return &items[index];
    }
    return NULL;
}

static home_ai_emergency_slot_t *find_event_locked(const char *event_id)
{
    home_ai_emergency_slot_t *items = slots();
    for (size_t index = 0U; index < HOME_AI_EMERGENCY_MAX_ACTIVE; ++index) {
        if (items[index].used && strcmp(items[index].event_id, event_id) == 0) return &items[index];
    }
    return NULL;
}

static bool ack_list_contains_locked(const char *event_id)
{
    for (size_t index = 0U; index < HOME_AI_EMERGENCY_ACKNOWLEDGEMENT_MAX; ++index) {
        if (s_acknowledgements[index].event_id[0] != '\0' &&
            strcmp(s_acknowledgements[index].event_id, event_id) == 0) return true;
    }
    return false;
}

static home_ai_emergency_slot_t *take_slot_locked(uint64_t now_ms)
{
    home_ai_emergency_slot_t *items = slots();
    home_ai_emergency_slot_t *oldest_resolved = NULL;
    for (size_t index = 0U; index < HOME_AI_EMERGENCY_MAX_ACTIVE; ++index) {
        if (!items[index].used) return &items[index];
        if (items[index].state == HOME_AI_EMERGENCY_RESOLVED &&
            now_ms >= items[index].resolved_at_ms &&
            now_ms - items[index].resolved_at_ms >= HOME_AI_EMERGENCY_RESOLVED_HOLD_MS) {
            oldest_resolved = &items[index];
        }
    }
    if (oldest_resolved != NULL) {
        memset(oldest_resolved, 0, sizeof(*oldest_resolved));
    }
    return oldest_resolved;
}

static void record_state(const home_ai_emergency_slot_t *slot, uint64_t now_ms)
{
    if (slot == NULL || !slot->used || slot->event_id[0] == '\0') return;
    home_ai_event_reporter_record_emergency(slot->event_id,
                                            slot->room_id,
                                            home_ai_emergency_state_name(slot->state),
                                            255U,
                                            now_ms);
}

static void prompt_for_slot(const home_ai_emergency_slot_t *slot,
                            char *out,
                            size_t out_size)
{
    if (slot == NULL || out == NULL || out_size == 0U) return;
    const bool recovering = slot->state == HOME_AI_EMERGENCY_RECOVERING ||
                            slot->state == HOME_AI_EMERGENCY_RESOLVED;
    (void)snprintf(out,
                   out_size,
                   recovering ? "Environmental risk in %s has recovered." :
                                 "Emergency environmental risk in %s. Please check now.",
                   slot->room_id[0] != '\0' ? slot->room_id : "the home");
}

static uint64_t next_repeat_at(const home_ai_emergency_slot_t *slot, uint64_t now_ms)
{
    if (slot->user_acknowledged) return now_ms + HOME_AI_EMERGENCY_ACK_REPEAT_MS;
    switch (slot->repeat_stage) {
    case 0U: return slot->schedule_base_ms + HOME_AI_EMERGENCY_INITIAL_REPEAT_MS;
    case 1U: return slot->schedule_base_ms + HOME_AI_EMERGENCY_SECOND_REPEAT_MS;
    case 2U: return slot->schedule_base_ms + HOME_AI_EMERGENCY_THIRD_REPEAT_MS;
    default: return now_ms + HOME_AI_EMERGENCY_REPEATED_REPEAT_MS;
    }
}

static bool should_dispatch(const home_ai_emergency_slot_t *slot, uint64_t now_ms)
{
    return slot != NULL && slot->used && !slot->dispatching &&
           slot->next_prompt_at_ms != 0U && now_ms >= slot->next_prompt_at_ms &&
           (slot->state == HOME_AI_EMERGENCY_DETECTED ||
            slot->state == HOME_AI_EMERGENCY_ACTIVE_UNACKNOWLEDGED ||
            slot->state == HOME_AI_EMERGENCY_ACKNOWLEDGED ||
            slot->state == HOME_AI_EMERGENCY_ESCALATED ||
            slot->state == HOME_AI_EMERGENCY_RECOVERING);
}

bool home_ai_emergency_coordinator_init(void)
{
    if (s_lock == NULL) s_lock = xSemaphoreCreateMutexStatic(&s_lock_storage);
    if (s_lock == NULL) return false;
#ifndef HOME_AI_EMERGENCY_HOST_TEST
    bool slots_created = false;
    if (s_slots == NULL) {
        s_slots = heap_caps_calloc(HOME_AI_EMERGENCY_MAX_ACTIVE,
                                   sizeof(*s_slots),
                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (s_slots == NULL) return false;
        slots_created = true;
    }
#endif
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(100U)) != pdTRUE) {
#ifndef HOME_AI_EMERGENCY_HOST_TEST
        if (slots_created) {
            heap_caps_free(s_slots);
            s_slots = NULL;
        }
#endif
        return false;
    }
    memset(slots(), 0, HOME_AI_EMERGENCY_MAX_ACTIVE * sizeof(*slots()));
    memset(s_acknowledgements, 0, sizeof(s_acknowledgements));
    s_initialized = true;
    xSemaphoreGive(s_lock);
    const esp_err_t observer_ret = environment_alarm_reporter_set_observer(
        home_ai_emergency_coordinator_on_alarm_event,
        NULL);
    if (observer_ret != ESP_OK) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_initialized = false;
        xSemaphoreGive(s_lock);
#ifndef HOME_AI_EMERGENCY_HOST_TEST
        if (slots_created) {
            heap_caps_free(s_slots);
            s_slots = NULL;
        }
#endif
        ESP_LOGE(TAG,
                 "observer registration failed ret=%s",
                 esp_err_to_name(observer_ret));
        return false;
    }
    ESP_LOGI(TAG, "initialized active_slots=%u repeat_schedule=1m/3m/5m",
             (unsigned int)HOME_AI_EMERGENCY_MAX_ACTIVE);
    return true;
}

void home_ai_emergency_coordinator_on_alarm_event(const alarm_event_t *event,
                                                  void *context)
{
    (void)context;
    if (!s_initialized || event == NULL || event->alarm_id == 0U || s_lock == NULL) return;
    const uint64_t now_ms = event->event_monotonic_ms != 0U ? event->event_monotonic_ms : monotonic_now_ms();
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(20U)) != pdTRUE) return;
    home_ai_emergency_slot_t *slot = find_alarm_locked(event->alarm_id);
    bool state_changed = false;
    if (event->status == ALARM_STATUS_ACTIVE) {
        if (event->alarm_level != ALARM_LEVEL_CRITICAL && slot == NULL) {
            xSemaphoreGive(s_lock);
            return;
        }
        if (slot == NULL) {
            slot = take_slot_locked(now_ms);
            if (slot == NULL) {
                xSemaphoreGive(s_lock);
                ESP_LOGW(TAG, "critical alarm dropped: fixed slot budget exhausted alarm_id=%016" PRIx64,
                         event->alarm_id);
                return;
            }
            memset(slot, 0, sizeof(*slot));
            slot->used = true;
            slot->alarm_id = event->alarm_id;
            slot->alarm_level = (uint8_t)event->alarm_level;
            slot->activated_at_ms = now_ms;
            slot->schedule_base_ms = now_ms;
            (void)snprintf(slot->event_id, sizeof(slot->event_id), "env_%016" PRIx64, event->alarm_id);
            copy_text(slot->room_id, sizeof(slot->room_id), event->room_id);
            slot->user_acknowledged = ack_list_contains_locked(slot->event_id);
            slot->state = slot->user_acknowledged ? HOME_AI_EMERGENCY_ACKNOWLEDGED :
                                                     HOME_AI_EMERGENCY_DETECTED;
            slot->next_prompt_at_ms = now_ms;
            state_changed = true;
        } else if (event->alarm_level > slot->alarm_level ||
                   event->reason == ALARM_REASON_LEVEL_ESCALATED ||
                   event->reason == ALARM_REASON_ESCALATED_TO_CRITICAL) {
            slot->alarm_level = (uint8_t)event->alarm_level;
            slot->state = HOME_AI_EMERGENCY_ESCALATED;
            slot->user_acknowledged = false;
            slot->repeat_stage = 0U;
            slot->schedule_base_ms = now_ms;
            slot->next_prompt_at_ms = now_ms;
            state_changed = true;
        }
    } else if (event->status == ALARM_STATUS_RECOVERED && slot != NULL) {
        slot->state = HOME_AI_EMERGENCY_RECOVERING;
        slot->dispatching = false;
        slot->playback_generation = 0U;
        slot->schedule_base_ms = now_ms;
        slot->next_prompt_at_ms = now_ms;
        state_changed = true;
    }
    home_ai_emergency_slot_t copy = {0};
    if (state_changed) copy = *slot;
    xSemaphoreGive(s_lock);
    if (state_changed) record_state(&copy, now_ms);
}

void home_ai_emergency_coordinator_tick(uint64_t now_ms)
{
    if (!s_initialized || now_ms == 0U || s_lock == NULL) return;
    size_t dispatched = 0U;
    for (size_t index = 0U; index < HOME_AI_EMERGENCY_MAX_ACTIVE && dispatched < HOME_AI_EMERGENCY_MAX_DISPATCH_PER_TICK; ++index) {
        home_ai_emergency_slot_t snapshot = {0};
        if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(20U)) != pdTRUE) return;
        home_ai_emergency_slot_t *slot = &slots()[index];
        if (!should_dispatch(slot, now_ms)) {
            if (slot->used && slot->state == HOME_AI_EMERGENCY_RESOLVED &&
                now_ms >= slot->resolved_at_ms &&
                now_ms - slot->resolved_at_ms >= HOME_AI_EMERGENCY_RESOLVED_HOLD_MS) {
                memset(slot, 0, sizeof(*slot));
            }
            xSemaphoreGive(s_lock);
            continue;
        }
        slot->dispatching = true;
        snapshot = *slot;
        xSemaphoreGive(s_lock);

        char prompt[128] = {0};
        prompt_for_slot(&snapshot, prompt, sizeof(prompt));
        home_ai_voice_route_result_t result = {0};
        const esp_err_t route_ret = home_ai_voice_router_request_prompt_with_event_id(
            snapshot.room_id[0] != '\0' ? snapshot.room_id : "living_room",
            prompt,
            "emergency_coordinator",
            true,
            snapshot.event_id,
            now_ms,
            &result);
        ++dispatched;

        if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(20U)) != pdTRUE) return;
        slot = find_event_locked(snapshot.event_id);
        if (slot != NULL) {
            slot->dispatching = false;
            if (route_ret == ESP_OK && result.queued_count > 0U) {
                slot->playback_generation = result.playback_generation;
                if (slot->state == HOME_AI_EMERGENCY_DETECTED) {
                    slot->state = HOME_AI_EMERGENCY_ACTIVE_UNACKNOWLEDGED;
                }
                slot->next_prompt_at_ms = next_repeat_at(slot, now_ms);
                if (!slot->user_acknowledged && slot->repeat_stage < 3U) ++slot->repeat_stage;
                snapshot = *slot;
            } else if (result.status == HOME_AI_VOICE_ROUTE_SUPPRESSED_NO_TERMINAL) {
                if (slot->state == HOME_AI_EMERGENCY_RECOVERING) {
                    slot->state = HOME_AI_EMERGENCY_RESOLVED;
                    slot->resolved_at_ms = now_ms;
                    slot->next_prompt_at_ms = 0U;
                } else {
                    if (slot->state == HOME_AI_EMERGENCY_DETECTED) {
                        slot->state = HOME_AI_EMERGENCY_ACTIVE_UNACKNOWLEDGED;
                    }
                    slot->next_prompt_at_ms = next_repeat_at(slot, now_ms);
                    if (!slot->user_acknowledged && slot->repeat_stage < 3U) ++slot->repeat_stage;
                }
                snapshot = *slot;
            } else {
                slot->next_prompt_at_ms = now_ms +
                    (slot->state == HOME_AI_EMERGENCY_RECOVERING ?
                         HOME_AI_EMERGENCY_RECOVERY_RETRY_MS : HOME_AI_EMERGENCY_RETRY_MS);
                snapshot = *slot;
            }
        }
        xSemaphoreGive(s_lock);
        if (slot != NULL) record_state(&snapshot, now_ms);
    }
}

bool home_ai_emergency_coordinator_playback_completed(const char *event_id,
                                                      uint32_t playback_generation,
                                                      bool ok,
                                                      uint64_t now_ms)
{
    if (!s_initialized || !valid_event_id(event_id) || playback_generation == 0U ||
        now_ms == 0U || s_lock == NULL) return false;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(20U)) != pdTRUE) return false;
    home_ai_emergency_slot_t *slot = find_event_locked(event_id);
    if (slot == NULL || slot->playback_generation != playback_generation) {
        xSemaphoreGive(s_lock);
        return false;
    }
    slot->playback_generation = 0U;
    if (slot->state == HOME_AI_EMERGENCY_RECOVERING) {
        if (ok) {
            slot->state = HOME_AI_EMERGENCY_RESOLVED;
            slot->resolved_at_ms = now_ms;
            slot->next_prompt_at_ms = 0U;
        } else {
            slot->next_prompt_at_ms = now_ms + HOME_AI_EMERGENCY_RECOVERY_RETRY_MS;
        }
    }
    home_ai_emergency_slot_t copy = *slot;
    xSemaphoreGive(s_lock);
    record_state(&copy, now_ms);
    return true;
}

bool home_ai_emergency_coordinator_acknowledge_user(const char *event_id,
                                                    uint64_t now_ms)
{
    if (!s_initialized || !valid_event_id(event_id) || now_ms == 0U || s_lock == NULL) return false;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(20U)) != pdTRUE) return false;
    home_ai_emergency_slot_t *slot = find_event_locked(event_id);
    if (slot == NULL || slot->state == HOME_AI_EMERGENCY_RESOLVED ||
        slot->state == HOME_AI_EMERGENCY_RECOVERING) {
        xSemaphoreGive(s_lock);
        return false;
    }
    slot->user_acknowledged = true;
    slot->state = HOME_AI_EMERGENCY_ACKNOWLEDGED;
    slot->next_prompt_at_ms = now_ms + HOME_AI_EMERGENCY_ACK_REPEAT_MS;
    home_ai_emergency_slot_t copy = *slot;
    xSemaphoreGive(s_lock);
    record_state(&copy, now_ms);
    return true;
}

bool home_ai_emergency_coordinator_replace_acknowledgements(
    const home_ai_emergency_acknowledgement_t *items,
    size_t count,
    uint64_t now_ms)
{
    if (!s_initialized) return true;
    if (count > HOME_AI_EMERGENCY_ACKNOWLEDGEMENT_MAX || s_lock == NULL) return false;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(20U)) != pdTRUE) return false;
    for (size_t index = 0U; index < count; ++index) {
        if (!valid_event_id(items[index].event_id)) {
            xSemaphoreGive(s_lock);
            return false;
        }
    }
    memset(s_acknowledgements, 0, sizeof(s_acknowledgements));
    for (size_t index = 0U; index < count; ++index) {
        s_acknowledgements[index] = items[index];
        home_ai_emergency_slot_t *slot = find_event_locked(items[index].event_id);
        if (slot != NULL && slot->state != HOME_AI_EMERGENCY_RESOLVED &&
            slot->state != HOME_AI_EMERGENCY_RECOVERING) {
            slot->user_acknowledged = true;
            slot->state = HOME_AI_EMERGENCY_ACKNOWLEDGED;
            slot->next_prompt_at_ms = now_ms + HOME_AI_EMERGENCY_ACK_REPEAT_MS;
        }
    }
    xSemaphoreGive(s_lock);
    return true;
}

size_t home_ai_emergency_coordinator_snapshot(home_ai_emergency_snapshot_t *out,
                                              size_t capacity)
{
    if (!s_initialized || out == NULL || capacity == 0U || s_lock == NULL) return 0U;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(20U)) != pdTRUE) return 0U;
    size_t count = 0U;
    for (size_t index = 0U; index < HOME_AI_EMERGENCY_MAX_ACTIVE && count < capacity; ++index) {
        const home_ai_emergency_slot_t *slot = &slots()[index];
        if (!slot->used) continue;
        home_ai_emergency_snapshot_t *item = &out[count++];
        memset(item, 0, sizeof(*item));
        item->used = true;
        item->alarm_id = slot->alarm_id;
        item->alarm_level = slot->alarm_level;
        item->state = slot->state;
        item->user_acknowledged = slot->user_acknowledged;
        item->playback_generation = slot->playback_generation;
        item->activated_at_ms = slot->activated_at_ms;
        item->next_prompt_at_ms = slot->next_prompt_at_ms;
        copy_text(item->event_id, sizeof(item->event_id), slot->event_id);
        copy_text(item->room_id, sizeof(item->room_id), slot->room_id);
    }
    xSemaphoreGive(s_lock);
    return count;
}

const char *home_ai_emergency_state_name(home_ai_emergency_state_t state)
{
    switch (state) {
    case HOME_AI_EMERGENCY_DETECTED: return "DETECTED";
    case HOME_AI_EMERGENCY_ACTIVE_UNACKNOWLEDGED: return "ACTIVE_UNACKNOWLEDGED";
    case HOME_AI_EMERGENCY_ACKNOWLEDGED: return "ACKNOWLEDGED";
    case HOME_AI_EMERGENCY_ESCALATED: return "ESCALATED";
    case HOME_AI_EMERGENCY_RECOVERING: return "RECOVERING";
    case HOME_AI_EMERGENCY_RESOLVED: return "RESOLVED";
    default: return "UNKNOWN";
    }
}
