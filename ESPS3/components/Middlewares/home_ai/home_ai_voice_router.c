#include "home_ai_voice_router.h"

#include <stdio.h>
#include <string.h>

#include "command_router.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "home_ai_event_reporter.h"
#include "home_ai_room_state.h"
#include "home_ai_voice_session.h"
#include "radar_registry.h"

#define HOME_AI_VOICE_ROUTER_REQUEST_TTL_MS 30000U
#define HOME_AI_VOICE_ROUTER_MIN_REPEAT_MS 30000U
#define HOME_AI_VOICE_ROUTER_GENERATION_START 1U

typedef struct {
    bool used;
    uint64_t expires_at_ms;
    uint32_t generation;
    uint8_t pending_ack_count;
    bool emergency;
    char emergency_event_id[64];
} home_ai_voice_inflight_t;

typedef struct {
    uint64_t mute_until_ms;
    uint64_t temporary_awake_until_ms;
    uint64_t last_prompt_ms;
} home_ai_voice_room_policy_t;

typedef struct {
    char device_id[HOME_AI_ROOM_STATE_VOICE_TERMINAL_ID_LEN];
    char room_id[HOME_AI_ROOM_STATE_ROOM_ID_LEN];
} home_ai_voice_target_t;

static home_ai_voice_inflight_t s_inflight[HOME_AI_VOICE_ROUTER_MAX_INFLIGHT];
static home_ai_voice_room_policy_t s_room_policy[HOME_AI_ROOM_STATE_COUNT];
static StaticSemaphore_t s_lock_storage;
static SemaphoreHandle_t s_lock;
static uint32_t s_playback_generation;
static bool s_initialized;

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

static bool valid_text(const char *value, size_t capacity)
{
    if (value == NULL || value[0] == '\0' || capacity < 2U) return false;
    for (size_t index = 0U; index < capacity; ++index) {
        if (value[index] == '\0') return true;
    }
    return false;
}

static int room_index(const char *room_id)
{
    if (room_id == NULL) return -1;
    for (radar_source_id_t source = RADAR_SOURCE_S3_LOCAL;
         source < RADAR_SOURCE_COUNT;
         source = (radar_source_id_t)(source + 1)) {
        home_ai_room_state_t state = {0};
        if (home_ai_room_state_get(source, &state) && strcmp(state.room_id, room_id) == 0) {
            return (int)source;
        }
    }
    return -1;
}

static radar_source_id_t source_for_room(const char *room_id)
{
    const int index = room_index(room_id);
    return index >= 0 ? (radar_source_id_t)index : RADAR_SOURCE_COUNT;
}

static bool lock_router(void)
{
    return s_lock != NULL && xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE;
}

static void unlock_router(void)
{
    (void)xSemaphoreGive(s_lock);
}

static bool json_escape(const char *input, char *out, size_t out_size)
{
    if (input == NULL || out == NULL || out_size == 0U) return false;
    size_t used = 0U;
    for (size_t index = 0U; input[index] != '\0'; ++index) {
        const unsigned char value = (unsigned char)input[index];
        const char *escape = NULL;
        char unicode[7] = {0};
        if (value == '"') escape = "\\\"";
        else if (value == '\\') escape = "\\\\";
        else if (value == '\n') escape = "\\n";
        else if (value == '\r') escape = "\\r";
        else if (value == '\t') escape = "\\t";
        else if (value < 0x20U) {
            (void)snprintf(unicode, sizeof(unicode), "\\u%04x", (unsigned int)value);
            escape = unicode;
        }
        if (escape != NULL) {
            const size_t length = strlen(escape);
            if (used + length + 1U > out_size) return false;
            memcpy(out + used, escape, length);
            used += length;
        } else {
            if (used + 2U > out_size) return false;
            out[used++] = (char)value;
        }
    }
    out[used] = '\0';
    return true;
}

static void result_init(home_ai_voice_route_result_t *result,
                        bool emergency,
                        const char *reason)
{
    if (result == NULL) return;
    memset(result, 0, sizeof(*result));
    result->status = HOME_AI_VOICE_ROUTE_REJECTED_INVALID;
    result->emergency = emergency;
    copy_text(result->reason, sizeof(result->reason), reason);
}

static bool room_allows_normal(const home_ai_room_state_t *state,
                               const home_ai_voice_room_policy_t *policy,
                               uint64_t now_ms,
                               home_ai_voice_route_status_t *out_status)
{
    if (state == NULL || policy == NULL || out_status == NULL) return false;
    if (state->presence_state != HOME_AI_ROOM_PRESENCE_OCCUPIED) {
        *out_status = HOME_AI_VOICE_ROUTE_SUPPRESSED_NO_PRESENCE;
        return false;
    }
    if (policy->mute_until_ms != 0U && now_ms < policy->mute_until_ms) {
        *out_status = HOME_AI_VOICE_ROUTE_SUPPRESSED_QUIET;
        return false;
    }
    if ((state->quiet_state == HOME_AI_ROOM_QUIET_SCHEDULED ||
         state->quiet_state == HOME_AI_ROOM_QUIET_SLEEP_CONFIRMED) &&
        (policy->temporary_awake_until_ms == 0U || now_ms >= policy->temporary_awake_until_ms)) {
        *out_status = HOME_AI_VOICE_ROUTE_SUPPRESSED_QUIET;
        return false;
    }
    if (policy->last_prompt_ms != 0U && now_ms >= policy->last_prompt_ms &&
        now_ms - policy->last_prompt_ms < HOME_AI_VOICE_ROUTER_MIN_REPEAT_MS) {
        *out_status = HOME_AI_VOICE_ROUTE_SUPPRESSED_RATE_LIMIT;
        return false;
    }
    return true;
}

static home_ai_voice_inflight_t *take_inflight_slot_locked(uint64_t now_ms)
{
    for (size_t index = 0U; index < HOME_AI_VOICE_ROUTER_MAX_INFLIGHT; ++index) {
        if (!s_inflight[index].used || now_ms >= s_inflight[index].expires_at_ms) {
            memset(&s_inflight[index], 0, sizeof(s_inflight[index]));
            s_inflight[index].used = true;
            s_inflight[index].expires_at_ms = now_ms + HOME_AI_VOICE_ROUTER_REQUEST_TTL_MS;
            return &s_inflight[index];
        }
    }
    return NULL;
}

static uint32_t next_generation_locked(void)
{
    ++s_playback_generation;
    if (s_playback_generation == 0U) {
        s_playback_generation = HOME_AI_VOICE_ROUTER_GENERATION_START;
    }
    return s_playback_generation;
}

static esp_err_t enqueue_prompt(const char *target_device_id,
                                const char *room_id,
                                const char *prompt,
                                const char *decision_id,
                                uint32_t generation,
                                bool emergency,
                                const char *event_id)
{
    char prompt_json[HOME_AI_VOICE_ROUTER_PROMPT_LEN * 2U];
    char room_json[HOME_AI_ROOM_STATE_ROOM_ID_LEN * 2U];
    char decision_json[HOME_AI_RULE_DECISION_ID_LEN * 2U];
    char event_json[64U * 2U];
    if (!json_escape(prompt, prompt_json, sizeof(prompt_json)) ||
        !json_escape(room_id, room_json, sizeof(room_json)) ||
        !json_escape(decision_id != NULL ? decision_id : "", decision_json, sizeof(decision_json)) ||
        !json_escape(event_id != NULL ? event_id : "", event_json, sizeof(event_json))) {
        return ESP_ERR_INVALID_SIZE;
    }
    char params[512];
    const int written = snprintf(params,
                                 sizeof(params),
                                 "{\"text\":\"%s\",\"room_id\":\"%s\","
                                 "\"decision_id\":\"%s\",\"playback_generation\":%lu,"
                                 "\"emergency_event_id\":\"%s\",\"emergency\":%u}",
                                 prompt_json,
                                 room_json,
                                 decision_json,
                                 (unsigned long)generation,
                                 event_json,
                                 emergency ? 1U : 0U);
    if (written <= 0 || written >= (int)sizeof(params)) return ESP_ERR_INVALID_SIZE;
    return command_router_enqueue(target_device_id,
                                  "speaker.play_audio",
                                  params,
                                  emergency ? "home_ai_emergency" : "home_ai_automation");
}

static bool emergency_target_allowed(const home_ai_room_state_t *state)
{
    return state != NULL && (state->presence_state == HOME_AI_ROOM_PRESENCE_OCCUPIED ||
                              state->presence_state == HOME_AI_ROOM_PRESENCE_UNKNOWN);
}

static bool resolve_target(radar_source_id_t source,
                           bool require_emergency_presence,
                           home_ai_voice_target_t *out_target)
{
    if (out_target == NULL) return false;
    memset(out_target, 0, sizeof(*out_target));
    home_ai_room_state_t state = {0};
    home_ai_room_state_config_t config = {0};
    if (!home_ai_room_state_get(source, &state) ||
        !home_ai_room_state_get_config(source, &config) ||
        (require_emergency_presence && !emergency_target_allowed(&state)) ||
        config.voice_terminal_device_id[0] == '\0') {
        return false;
    }
    copy_text(out_target->device_id,
              sizeof(out_target->device_id),
              config.voice_terminal_device_id);
    copy_text(out_target->room_id, sizeof(out_target->room_id), config.room_id);
    return true;
}

bool home_ai_voice_router_init(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutexStatic(&s_lock_storage);
        if (s_lock == NULL) return false;
    }
    if (!lock_router()) return false;
    memset(s_inflight, 0, sizeof(s_inflight));
    memset(s_room_policy, 0, sizeof(s_room_policy));
    s_playback_generation = HOME_AI_VOICE_ROUTER_GENERATION_START - 1U;
    s_initialized = true;
    unlock_router();
    return true;
}

void home_ai_voice_router_tick(uint64_t now_ms)
{
    if (now_ms == 0U || !lock_router()) return;
    if (!s_initialized) {
        unlock_router();
        return;
    }
    bool clear_temporary_awake[HOME_AI_ROOM_STATE_COUNT] = {false};
    for (size_t index = 0U; index < HOME_AI_VOICE_ROUTER_MAX_INFLIGHT; ++index) {
        if (s_inflight[index].used && now_ms >= s_inflight[index].expires_at_ms) {
            memset(&s_inflight[index], 0, sizeof(s_inflight[index]));
        }
    }
    for (size_t index = 0U; index < HOME_AI_ROOM_STATE_COUNT; ++index) {
        if (s_room_policy[index].temporary_awake_until_ms != 0U &&
            now_ms >= s_room_policy[index].temporary_awake_until_ms) {
            s_room_policy[index].temporary_awake_until_ms = 0U;
            clear_temporary_awake[index] = true;
        }
    }
    unlock_router();
    for (size_t index = 0U; index < HOME_AI_ROOM_STATE_COUNT; ++index) {
        if (clear_temporary_awake[index]) {
            (void)home_ai_room_state_set_quiet_state((radar_source_id_t)index,
                                                     HOME_AI_ROOM_QUIET_NORMAL,
                                                     now_ms);
        }
    }
}

bool home_ai_voice_router_acknowledge_ex(uint32_t playback_generation,
                                         home_ai_voice_ack_result_t *out_result)
{
    if (out_result != NULL) memset(out_result, 0, sizeof(*out_result));
    if (playback_generation == 0U || !lock_router()) return false;
    bool matched = false;
    if (s_initialized) {
        for (size_t index = 0U; index < HOME_AI_VOICE_ROUTER_MAX_INFLIGHT; ++index) {
            home_ai_voice_inflight_t *slot = &s_inflight[index];
            if (!slot->used || slot->generation != playback_generation ||
                slot->pending_ack_count == 0U) {
                continue;
            }
            --slot->pending_ack_count;
            matched = true;
            if (out_result != NULL) {
                out_result->matched = true;
                out_result->emergency = slot->emergency;
                copy_text(out_result->emergency_event_id,
                          sizeof(out_result->emergency_event_id),
                          slot->emergency_event_id);
            }
            if (slot->pending_ack_count == 0U) {
                if (out_result != NULL) out_result->completed = true;
                memset(slot, 0, sizeof(*slot));
            }
            break;
        }
    }
    unlock_router();
    return matched;
}

bool home_ai_voice_router_acknowledge(uint32_t playback_generation)
{
    return home_ai_voice_router_acknowledge_ex(playback_generation, NULL);
}

esp_err_t home_ai_voice_router_request_stop(const char *room_id)
{
    if (!s_initialized || !valid_text(room_id, HOME_AI_ROOM_STATE_ROOM_ID_LEN)) {
        return ESP_ERR_INVALID_ARG;
    }
    const radar_source_id_t source = source_for_room(room_id);
    home_ai_voice_target_t target = {0};
    if (source >= RADAR_SOURCE_COUNT || !resolve_target(source, false, &target)) {
        return ESP_ERR_NOT_FOUND;
    }
    home_ai_voice_session_lease_t preempted = {0};
    (void)home_ai_voice_session_preempt_for_emergency(&preempted);
    return command_router_enqueue(target.device_id,
                                  "speaker.stop_audio",
                                  "{\"reason\":\"offline_voice_stop\"}",
                                  "home_ai_offline_voice");
}

esp_err_t home_ai_voice_router_request_prompt_with_event_id(
    const char *room_id,
    const char *prompt,
    const char *decision_id,
    bool emergency,
    const char *emergency_event_id,
    uint64_t now_ms,
    home_ai_voice_route_result_t *out_result)
{
    result_init(out_result, emergency, "invalid_request");
    if (out_result == NULL || !s_initialized || now_ms == 0U ||
        !valid_text(room_id, HOME_AI_ROOM_STATE_ROOM_ID_LEN) ||
        !valid_text(prompt, HOME_AI_VOICE_ROUTER_PROMPT_LEN)) return ESP_ERR_INVALID_ARG;
    home_ai_voice_router_tick(now_ms);

    const char *event_id = "";
    char generated_event_id[64] = {0};
    if (emergency) {
        if (emergency_event_id != NULL && emergency_event_id[0] != '\0') {
            if (!valid_text(emergency_event_id, sizeof(generated_event_id))) return ESP_ERR_INVALID_ARG;
            event_id = emergency_event_id;
        } else {
            (void)snprintf(generated_event_id,
                           sizeof(generated_event_id),
                           "emergency_voice_%lu_%08lx",
                           (unsigned long)now_ms,
                           (unsigned long)esp_random());
            event_id = generated_event_id;
        }
    }

    const radar_source_id_t primary_source = source_for_room(room_id);
    if (primary_source >= RADAR_SOURCE_COUNT) return ESP_ERR_INVALID_ARG;
    home_ai_room_state_t primary_state = {0};
    if (!home_ai_room_state_get(primary_source, &primary_state)) return ESP_ERR_INVALID_STATE;
    home_ai_voice_target_t targets[HOME_AI_ROOM_STATE_COUNT] = {0};
    size_t target_count = 0U;
    if (!emergency || primary_source != RADAR_SOURCE_S3_LOCAL) {
        if (resolve_target(primary_source, emergency, &targets[target_count])) {
            ++target_count;
        }
    } else {
        for (radar_source_id_t source = RADAR_SOURCE_C51;
             source < RADAR_SOURCE_COUNT && target_count < HOME_AI_ROOM_STATE_COUNT;
             source = (radar_source_id_t)(source + 1)) {
            if (resolve_target(source, true, &targets[target_count])) {
                ++target_count;
            }
        }
    }

    if (!emergency) {
        const int index = (int)primary_source;
        if (!lock_router()) return ESP_ERR_INVALID_STATE;
        const bool allowed = s_initialized &&
                             room_allows_normal(&primary_state,
                                                &s_room_policy[index],
                                                now_ms,
                                                &out_result->status);
        unlock_router();
        if (!allowed) {
            copy_text(out_result->reason,
                      sizeof(out_result->reason),
                      home_ai_voice_route_status_name(out_result->status));
            return ESP_OK;
        }
    }

    if (target_count == 0U) {
        out_result->status = HOME_AI_VOICE_ROUTE_SUPPRESSED_NO_TERMINAL;
        copy_text(out_result->reason,
                  sizeof(out_result->reason),
                  home_ai_voice_route_status_name(out_result->status));
        return ESP_OK;
    }

    if (!emergency) {
        home_ai_voice_session_lease_t active = {0};
        if (home_ai_voice_session_get(&active)) {
            out_result->status = HOME_AI_VOICE_ROUTE_SUPPRESSED_BUSY;
            copy_text(out_result->reason, sizeof(out_result->reason), "voice_session_busy");
            return ESP_OK;
        }
    }

    if (!lock_router()) return ESP_ERR_INVALID_STATE;
    home_ai_voice_inflight_t *slot = take_inflight_slot_locked(now_ms);
    if (slot == NULL) {
        unlock_router();
        out_result->status = HOME_AI_VOICE_ROUTE_REJECTED_RESOURCE;
        copy_text(out_result->reason, sizeof(out_result->reason), "voice_request_budget_exhausted");
        return ESP_ERR_NO_MEM;
    }
    slot->generation = next_generation_locked();
    slot->emergency = emergency;
    copy_text(slot->emergency_event_id, sizeof(slot->emergency_event_id), event_id);
    out_result->playback_generation = slot->generation;
    out_result->queued_count = 0U;
    for (size_t index = 0U; index < target_count; ++index) {
        esp_err_t ret = enqueue_prompt(targets[index].device_id,
                                       targets[index].room_id,
                                       prompt,
                                       decision_id,
                                       slot->generation,
                                       emergency,
                                       event_id);
        if (ret == ESP_OK) {
            ++slot->pending_ack_count;
            ++out_result->queued_count;
        }
    }
    if (out_result->queued_count == 0U) {
        memset(slot, 0, sizeof(*slot));
        out_result->playback_generation = 0U;
        unlock_router();
        out_result->status = HOME_AI_VOICE_ROUTE_REJECTED_RESOURCE;
        copy_text(out_result->reason, sizeof(out_result->reason), "command_queue_rejected");
        return ESP_ERR_NO_MEM;
    }
    out_result->status = HOME_AI_VOICE_ROUTE_QUEUED;
    copy_text(out_result->reason, sizeof(out_result->reason), emergency ? "emergency_preempt_queued" : "automation_prompt_queued");
    copy_text(out_result->emergency_event_id, sizeof(out_result->emergency_event_id), event_id);
    if (!emergency) {
        s_room_policy[primary_source].last_prompt_ms = now_ms;
    }
    unlock_router();
    if (emergency) {
        home_ai_voice_session_lease_t preempted = {0};
        (void)home_ai_voice_session_preempt_for_emergency(&preempted);
    }
    return ESP_OK;
}

esp_err_t home_ai_voice_router_request_prompt(const char *room_id,
                                              const char *prompt,
                                              const char *decision_id,
                                              bool emergency,
                                              uint64_t now_ms,
                                              home_ai_voice_route_result_t *out_result)
{
    return home_ai_voice_router_request_prompt_with_event_id(room_id,
                                                              prompt,
                                                              decision_id,
                                                              emergency,
                                                              NULL,
                                                              now_ms,
                                                              out_result);
}

bool home_ai_voice_router_set_mute(const char *room_id, bool muted, uint64_t until_ms)
{
    const int index = room_index(room_id);
    if (index < 0 || !lock_router()) return false;
    s_room_policy[index].mute_until_ms = muted ? (until_ms == 0U ? UINT64_MAX : until_ms) : 0U;
    unlock_router();
    return true;
}

bool home_ai_voice_router_set_temporary_awake(const char *room_id,
                                              uint64_t duration_ms,
                                              uint64_t now_ms)
{
    const int index = room_index(room_id);
    home_ai_room_state_t state = {0};
    if (index < 0 || now_ms == 0U || duration_ms == 0U ||
        !home_ai_room_state_get((radar_source_id_t)index, &state)) return false;
    if (state.quiet_state == HOME_AI_ROOM_QUIET_NORMAL) return true;
    if (!lock_router()) return false;
    s_room_policy[index].temporary_awake_until_ms = now_ms + duration_ms;
    unlock_router();
    if (home_ai_room_state_set_quiet_state((radar_source_id_t)index,
                                           HOME_AI_ROOM_QUIET_TEMPORARY_AWAKE,
                                           now_ms)) {
        return true;
    }
    if (lock_router()) {
        s_room_policy[index].temporary_awake_until_ms = 0U;
        unlock_router();
    }
    return false;
}

bool home_ai_voice_router_clear_temporary_awake(const char *room_id, uint64_t now_ms)
{
    const int index = room_index(room_id);
    if (index < 0 || now_ms == 0U || !lock_router()) return false;
    s_room_policy[index].temporary_awake_until_ms = 0U;
    unlock_router();
    return home_ai_room_state_set_quiet_state((radar_source_id_t)index,
                                              HOME_AI_ROOM_QUIET_NORMAL,
                                              now_ms);
}

const char *home_ai_voice_route_status_name(home_ai_voice_route_status_t status)
{
    switch (status) {
    case HOME_AI_VOICE_ROUTE_QUEUED: return "queued";
    case HOME_AI_VOICE_ROUTE_SUPPRESSED_NO_PRESENCE: return "suppressed_no_presence";
    case HOME_AI_VOICE_ROUTE_SUPPRESSED_QUIET: return "suppressed_quiet";
    case HOME_AI_VOICE_ROUTE_SUPPRESSED_BUSY: return "suppressed_voice_busy";
    case HOME_AI_VOICE_ROUTE_SUPPRESSED_RATE_LIMIT: return "suppressed_rate_limit";
    case HOME_AI_VOICE_ROUTE_SUPPRESSED_NO_TERMINAL: return "suppressed_no_terminal";
    case HOME_AI_VOICE_ROUTE_REJECTED_RESOURCE: return "rejected_resource";
    case HOME_AI_VOICE_ROUTE_REJECTED_INVALID: return "rejected_invalid";
    default: return "unknown";
    }
}
