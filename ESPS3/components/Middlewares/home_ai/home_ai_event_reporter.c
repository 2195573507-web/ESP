#include "home_ai_event_reporter.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "gateway_config.h"
#include "network_replay_worker.h"
#include "network_worker.h"

static const char *TAG = "home_ai_events";
static bool s_initialized;
static uint64_t s_last_capacity_report_ms;
static uint8_t s_last_capacity_percent;
static bool s_last_capacity_warning;
static home_ai_history_stats_t s_last_reported_stats;
static bool s_have_last_reported_stats;

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

static bool json_escape(const char *input, char *out, size_t out_size)
{
    if (input == NULL || out == NULL || out_size == 0U) return false;
    size_t used = 0U;
    for (size_t index = 0U; input[index] != '\0'; ++index) {
        const unsigned char value = (unsigned char)input[index];
        const char *escape = NULL;
        char escaped[7] = {0};
        if (value == '"') escape = "\\\"";
        else if (value == '\\') escape = "\\\\";
        else if (value == '\n') escape = "\\n";
        else if (value == '\r') escape = "\\r";
        else if (value == '\t') escape = "\\t";
        else if (value < 0x20U) {
            (void)snprintf(escaped, sizeof(escaped), "\\u%04x", (unsigned int)value);
            escape = escaped;
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

static uint8_t event_priority(uint16_t priority)
{
    return priority > 255U ? 255U : (uint8_t)priority;
}

static bool enqueue_event(const char *event_id,
                          const char *event_type,
                          const char *room_id,
                          uint16_t priority,
                          uint64_t occurred_at_ms,
                          const char *payload)
{
    if (!s_initialized || event_id == NULL || event_type == NULL || payload == NULL ||
        event_id[0] == '\0' || event_type[0] == '\0' || occurred_at_ms == 0U) return false;

    home_ai_history_event_t event = {0};
    event.valid = true;
    event.sequence = 0U;
    event.priority = event_priority(priority);
    event.occurred_at_ms = occurred_at_ms;
    copy_text(event.event_id, sizeof(event.event_id), event_id);
    copy_text(event.event_type, sizeof(event.event_type), event_type);
    copy_text(event.room_id, sizeof(event.room_id), room_id != NULL ? room_id : "");
    copy_text(event.payload, sizeof(event.payload), payload);
    event.payload_len = (uint16_t)strlen(event.payload);
    if (event.payload_len >= HOME_AI_HISTORY_PAYLOAD_BYTES) {
        ESP_LOGW(TAG, "event rejected payload_too_large event_id=%s", event_id);
        return false;
    }
    const esp_err_t history_ret = home_ai_history_enqueue(&event);
    if (history_ret != ESP_OK) {
        ESP_LOGW(TAG,
                 "event history enqueue failed event_id=%s ret=%s",
                 event_id,
                 esp_err_to_name(history_ret));
    }

    char event_id_json[HOME_AI_HISTORY_EVENT_ID_LEN * 2U];
    char event_type_json[HOME_AI_HISTORY_EVENT_TYPE_LEN * 2U];
    char room_id_json[HOME_AI_HISTORY_ROOM_ID_LEN * 2U];
    char gateway_id_json[128];
    if (!json_escape(event.event_id, event_id_json, sizeof(event_id_json)) ||
        !json_escape(event.event_type, event_type_json, sizeof(event_type_json)) ||
        !json_escape(event.room_id, room_id_json, sizeof(room_id_json)) ||
        !json_escape(gateway_config_get()->gateway_id, gateway_id_json, sizeof(gateway_id_json))) {
        return history_ret == ESP_OK;
    }
    char body[1536];
    const int written = snprintf(
        body,
        sizeof(body),
        "{\"gateway_id\":\"%s\",\"events\":[{\"event_id\":\"%s\","
        "\"event_type\":\"%s\",\"room_id\":\"%s\",\"priority\":%u,"
        "\"occurred_at_ms\":%llu,\"source_device_id\":\"%s\","
        "\"sequence_no\":%llu,\"schema_version\":1,\"payload\":%s}]}",
        gateway_id_json,
        event_id_json,
        event_type_json,
        room_id_json,
        (unsigned int)priority,
        (unsigned long long)occurred_at_ms,
        gateway_id_json,
        (unsigned long long)occurred_at_ms,
        payload);
    if (written <= 0 || written >= (int)sizeof(body)) return history_ret == ESP_OK;
    char *owned = cJSON_malloc((size_t)written + 1U);
    if (owned == NULL) return history_ret == ESP_OK;
    memcpy(owned, body, (size_t)written + 1U);
    const esp_err_t submit_ret = network_worker_submit_home_ai_events(owned, "home_ai_event");
    if (submit_ret != ESP_OK) cJSON_free(owned);
    network_replay_worker_request_home_ai_replay();
    return history_ret == ESP_OK || submit_ret == ESP_OK;
}

bool home_ai_event_reporter_init(void)
{
    s_initialized = home_ai_history_store_init();
    s_last_capacity_report_ms = 0U;
    s_last_capacity_percent = 0U;
    s_last_capacity_warning = false;
    memset(&s_last_reported_stats, 0, sizeof(s_last_reported_stats));
    s_have_last_reported_stats = false;
    return s_initialized;
}

void home_ai_event_reporter_tick(uint64_t occurred_at_ms)
{
    if (!s_initialized || occurred_at_ms == 0U) return;
    const home_ai_history_stats_t stats = home_ai_history_get_stats();
    const bool warning_changed = stats.capacity_warning != s_last_capacity_warning;
    const bool capacity_changed = stats.capacity_percent >= HOME_AI_HISTORY_CAPACITY_WARNING_PERCENT &&
                                   (stats.capacity_percent >= s_last_capacity_percent + 5U ||
                                    stats.capacity_percent + 5U <= s_last_capacity_percent);
    const bool pressure_nonzero = stats.dropped_unpersisted != 0U ||
                                  stats.dropped_overwrite != 0U ||
                                  stats.storage_errors != 0U ||
                                  stats.retention_evictions != 0U ||
                                  stats.protected_rejections != 0U;
    const bool pressure_changed = s_have_last_reported_stats ?
        (stats.dropped_unpersisted != s_last_reported_stats.dropped_unpersisted ||
         stats.dropped_overwrite != s_last_reported_stats.dropped_overwrite ||
         stats.storage_errors != s_last_reported_stats.storage_errors ||
         stats.retention_evictions != s_last_reported_stats.retention_evictions ||
         stats.protected_rejections != s_last_reported_stats.protected_rejections) :
        pressure_nonzero;
    const bool first_report = s_last_capacity_report_ms == 0U;
    const bool due = first_report || occurred_at_ms - s_last_capacity_report_ms >= 60000U;
    if (!due || (!warning_changed && !capacity_changed &&
                 !pressure_changed && !(first_report && stats.capacity_warning))) return;

    char payload[512];
    const int written = snprintf(payload,
                                 sizeof(payload),
                                 "{\"capacity_percent\":%u,\"capacity_warning\":%s,"
                                 "\"persisted_count\":%lu,\"unuploaded_count\":%lu,"
                                 "\"pending_ram_count\":%lu,\"dropped_unpersisted\":%lu,"
                                 "\"dropped_overwrite\":%lu,\"storage_errors\":%lu,"
                                 "\"retention_evictions\":%lu,"
                                 "\"protected_rejections\":%lu}",
                                 (unsigned int)stats.capacity_percent,
                                 stats.capacity_warning ? "true" : "false",
                                 (unsigned long)stats.persisted_count,
                                 (unsigned long)stats.unuploaded_count,
                                 (unsigned long)stats.pending_ram_count,
                                 (unsigned long)stats.dropped_unpersisted,
                                 (unsigned long)stats.dropped_overwrite,
                                 (unsigned long)stats.storage_errors,
                                 (unsigned long)stats.retention_evictions,
                                 (unsigned long)stats.protected_rejections);
    if (written <= 0 || written >= (int)sizeof(payload)) return;

    char event_id[HOME_AI_HISTORY_EVENT_ID_LEN];
    (void)snprintf(event_id,
                   sizeof(event_id),
                   "offline_buffer_%llu",
                   (unsigned long long)occurred_at_ms);
    /* Update the edge state before enqueueing so a failed upload cannot recurse. */
    s_last_capacity_report_ms = occurred_at_ms;
    s_last_capacity_percent = stats.capacity_percent;
    s_last_capacity_warning = stats.capacity_warning;
    s_last_reported_stats = stats;
    s_have_last_reported_stats = true;
    (void)enqueue_event(event_id, "offline_buffer", "", 700U, occurred_at_ms, payload);
}

void home_ai_event_reporter_record_decision(const home_ai_rule_decision_t *decision,
                                            const home_ai_virtual_device_execution_t *execution,
                                            uint64_t occurred_at_ms)
{
    if (decision == NULL) return;
    char rule_id[HOME_AI_RULE_ID_LEN * 2U];
    char device_id[HOME_AI_RULE_DEVICE_ID_LEN * 2U];
    char override_id[HOME_AI_OVERRIDE_ID_LEN * 2U];
    if (!json_escape(decision->rule_id, rule_id, sizeof(rule_id)) ||
        !json_escape(decision->device_id, device_id, sizeof(device_id)) ||
        !json_escape(decision->suppression_override_id, override_id, sizeof(override_id))) return;
    const char *execution_name = execution != NULL ?
        home_ai_virtual_execution_result_name(execution->result) : "not_executed";
    const char *reason = execution != NULL ? execution->reason :
                                             home_ai_rule_decision_state_name(decision->state);
    char reason_json[128];
    if (!json_escape(reason, reason_json, sizeof(reason_json))) return;
    char payload[768];
    const int written = snprintf(payload,
                                 sizeof(payload),
                                 "{\"rule_id\":\"%s\",\"device_id\":\"%s\","
                                 "\"action\":\"%s\",\"decision_state\":\"%s\","
                                 "\"execution_result\":\"%s\",\"reason\":\"%s\","
                                 "\"override_id\":\"%s\",\"execution_mode\":\"virtual\","
                                 "\"verified\":%s}",
                                 rule_id,
                                 device_id,
                                 home_ai_rule_action_name(decision->action),
                                 home_ai_rule_decision_state_name(decision->state),
                                 execution_name,
                                 reason_json,
                                 override_id,
                                 execution != NULL && execution->state.verified ? "true" : "false");
    if (written <= 0 || written >= (int)sizeof(payload)) return;
    const char *event_type = decision->state == HOME_AI_RULE_DECISION_EXECUTE ?
                                 "decision" : "suppressed_action";
    (void)enqueue_event(decision->decision_id,
                        event_type,
                        decision->room_id,
                        decision->priority,
                        occurred_at_ms,
                        payload);
}

void home_ai_event_reporter_record_virtual_state(const home_ai_virtual_device_state_t *state,
                                                 uint64_t occurred_at_ms)
{
    if (state == NULL || !state->valid) return;
    char device_id[HOME_AI_RULE_DEVICE_ID_LEN * 2U];
    if (!json_escape(state->device_id, device_id, sizeof(device_id))) return;
    char event_id[HOME_AI_HISTORY_EVENT_ID_LEN];
    (void)snprintf(event_id, sizeof(event_id), "state_%s_%llu", state->device_id,
                   (unsigned long long)occurred_at_ms);
    char payload[768];
    const int written = snprintf(payload,
                                 sizeof(payload),
                                 "{\"device_id\":\"%s\",\"device_type\":\"%s\","
                                 "\"power\":\"%s\",\"execution_mode\":\"virtual\","
                                 "\"verified\":%s,\"last_action\":\"%s\","
                                 "\"decision_id\":\"%s\"}",
                                 device_id,
                                 home_ai_rule_device_type_name(state->device_type),
                                 home_ai_virtual_power_name(state->power),
                                 state->verified ? "true" : "false",
                                 state->last_action,
                                 state->decision_id);
    if (written > 0 && written < (int)sizeof(payload)) {
        (void)enqueue_event(event_id,
                            "virtual_device_state",
                            state->room_id,
                            500U,
                            occurred_at_ms,
                            payload);
    }

    char room_id[HOME_AI_RULE_ROOM_ID_LEN * 2U];
    char action_source[96];
    char decision_id[HOME_AI_RULE_DECISION_ID_LEN * 2U];
    char decision_reason[192];
    if (!json_escape(state->room_id, room_id, sizeof(room_id)) ||
        !json_escape(state->action_source, action_source, sizeof(action_source)) ||
        !json_escape(state->decision_id, decision_id, sizeof(decision_id)) ||
        !json_escape(state->decision_reason, decision_reason, sizeof(decision_reason))) return;
    char body[1200];
    const int body_written = snprintf(
        body,
        sizeof(body),
        "{\"devices\":[{\"device_id\":\"%s\",\"room_id\":\"%s\","
        "\"device_type\":\"%s\",\"power\":\"%s\",\"execution_mode\":\"virtual\","
        "\"last_action\":\"%s\",\"action_source\":\"%s\","
        "\"decision_id\":\"%s\",\"decision_reason\":\"%s\","
        "\"verified\":%s,\"updated_at_ms\":%llu}]}",
        device_id,
        room_id,
        home_ai_rule_device_type_name(state->device_type),
        home_ai_virtual_power_name(state->power),
        state->last_action,
        action_source,
        decision_id,
        decision_reason,
        state->verified ? "true" : "false",
        (unsigned long long)state->updated_at_ms);
    if (body_written <= 0 || body_written >= (int)sizeof(body)) return;
    char *owned = cJSON_malloc((size_t)body_written + 1U);
    if (owned == NULL) return;
    memcpy(owned, body, (size_t)body_written + 1U);
    const esp_err_t submit_ret = network_worker_submit_home_ai_virtual_state(owned,
                                                                             "home_ai_state");
    if (submit_ret != ESP_OK) cJSON_free(owned);
}

void home_ai_event_reporter_record_room_state(const home_ai_room_state_t *state,
                                              uint64_t occurred_at_ms)
{
    if (state == NULL) return;
    char event_id[HOME_AI_HISTORY_EVENT_ID_LEN];
    (void)snprintf(event_id,
                   sizeof(event_id),
                   "room_%s_%llu",
                   state->room_id,
                   (unsigned long long)occurred_at_ms);
    char payload[640];
    const int written = snprintf(payload,
                                 sizeof(payload),
                                 "{\"presence_state\":\"%s\",\"stable_target_count\":%u,"
                                 "\"occupancy_mode\":\"%s\",\"environment_fresh\":%s,"
                                 "\"radar_fresh\":%s}",
                                 home_ai_room_presence_state_name(state->presence_state),
                                 (unsigned int)state->stable_target_count,
                                 home_ai_room_occupancy_mode_name(state->occupancy_mode),
                                 state->environment_fresh ? "true" : "false",
                                 state->radar_fresh ? "true" : "false");
    if (written > 0 && written < (int)sizeof(payload)) {
        (void)enqueue_event(event_id, "room_state", state->room_id, 400U, occurred_at_ms, payload);
    }
}

void home_ai_event_reporter_record_rule_activation(
    const home_ai_rule_activation_result_t *activation,
    uint64_t occurred_at_ms)
{
    if (activation == NULL) return;
    char payload[768];
    const int written = snprintf(payload,
                                 sizeof(payload),
                                 "{\"state\":\"%s\",\"package_version\":%lu,"
                                 "\"active_rule_count\":%lu,\"accepted_count\":%lu,"
                                 "\"rejected_count\":%lu,\"error_code\":\"%s\"}",
                                 home_ai_rule_activation_state_name(activation->state),
                                 (unsigned long)activation->package_version,
                                 (unsigned long)activation->active_rule_count,
                                 (unsigned long)activation->accepted_count,
                                 (unsigned long)activation->rejected_count,
                                 activation->error_code);
    if (written <= 0 || written >= (int)sizeof(payload)) return;
    char event_id[HOME_AI_HISTORY_EVENT_ID_LEN];
    (void)snprintf(event_id,
                   sizeof(event_id),
                   "rule_sync_%lu_%llu",
                   (unsigned long)activation->package_version,
                   (unsigned long long)occurred_at_ms);
    (void)enqueue_event(event_id, "rule_sync", "", 600U, occurred_at_ms, payload);
}

void home_ai_event_reporter_record_user_override(const home_ai_user_override_t *override,
                                                 const char *device_id,
                                                 uint64_t occurred_at_ms)
{
    if (override == NULL || occurred_at_ms == 0U) return;
    char override_id[HOME_AI_OVERRIDE_ID_LEN * 2U];
    char device_id_json[HOME_AI_RULE_DEVICE_ID_LEN * 2U];
    if (!json_escape(override->override_id, override_id, sizeof(override_id)) ||
        !json_escape(device_id != NULL ? device_id : override->device_id,
                     device_id_json,
                     sizeof(device_id_json))) return;
    char payload[640];
    const int written = snprintf(payload,
                                 sizeof(payload),
                                 "{\"feedback_type\":\"manual_override\","
                                 "\"override_id\":\"%s\",\"device_id\":\"%s\","
                                 "\"action\":\"%s\",\"expires_at_ms\":%llu}",
                                 override_id,
                                 device_id_json,
                                 home_ai_override_action_name(override->action),
                                 (unsigned long long)override->expires_at_ms);
    if (written > 0 && written < (int)sizeof(payload)) {
        (void)enqueue_event(override->override_id,
                            "feedback",
                            override->room_id,
                            override->priority,
                            occurred_at_ms,
                            payload);
    }
}

void home_ai_event_reporter_record_emergency(const char *event_id,
                                             const char *room_id,
                                             const char *state,
                                             uint8_t priority,
                                             uint64_t occurred_at_ms)
{
    char state_json[96];
    if (event_id == NULL || state == NULL || !json_escape(state, state_json, sizeof(state_json))) return;
    char payload[192];
    const int written = snprintf(payload, sizeof(payload), "{\"state\":\"%s\"}", state_json);
    if (written > 0 && written < (int)sizeof(payload)) {
        (void)enqueue_event(event_id, "emergency", room_id, priority, occurred_at_ms, payload);
    }
}

void home_ai_event_reporter_record_playback_ack(const char *event_id,
                                                const char *room_id,
                                                uint32_t playback_generation,
                                                bool emergency,
                                                bool ok,
                                                uint64_t occurred_at_ms)
{
    if (event_id == NULL || event_id[0] == '\0' || occurred_at_ms == 0U) return;
    char payload[256];
    const int written = snprintf(payload,
                                 sizeof(payload),
                                 "{\"playback_generation\":%lu,\"emergency\":%s,\"ok\":%s}",
                                 (unsigned long)playback_generation,
                                 emergency ? "true" : "false",
                                 ok ? "true" : "false");
    if (written > 0 && written < (int)sizeof(payload)) {
        (void)enqueue_event(event_id,
                            "playback_ack",
                            room_id,
                            emergency ? 1000U : 500U,
                            occurred_at_ms,
                            payload);
    }
}
