#include "radar_gateway_ingest.h"

#include <ctype.h>
#include <limits.h>
#include <math.h>
#include <string.h>

#include "cJSON.h"
#include "radar_coordinate_transform.h"
#include "radar_rate_manager.h"
#include "radar_target_tracker.h"
#include "radar_zone_map.h"

#ifndef RADAR_GATEWAY_HOST_TEST
#include "radar_log_manager.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "child_registry.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "protocol_adapter.h"
#include "resource_manager.h"
#else
#include <stdio.h>
#define ESP_LOGI(tag, format, ...) ((void)(tag))
#endif

static const char *TAG = "radar_gateway";

typedef struct {
    bool has_sample;
    uint8_t local_id;
    radar_source_id_t source;
    uint32_t session_generation;
    uint32_t last_sequence;
    uint64_t last_received_ms;
    uint64_t last_log_ms;
    radar_gateway_sample_t last_sample;
    radar_spatial_state_t spatial;
    radar_rate_manager_t rate_manager;
    radar_gateway_output_t output;
} radar_gateway_slot_t;

static radar_gateway_slot_t s_slots[RADAR_GATEWAY_MAX_REMOTE_SOURCES];
static bool s_initialized;
#ifndef RADAR_GATEWAY_HOST_TEST
static StaticSemaphore_t s_lock_storage;
static SemaphoreHandle_t s_lock;
#endif

static bool gateway_lock(void)
{
#ifdef RADAR_GATEWAY_HOST_TEST
    return true;
#else
    return s_lock != NULL && xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE;
#endif
}

static void gateway_unlock(void)
{
#ifndef RADAR_GATEWAY_HOST_TEST
    xSemaphoreGive(s_lock);
#endif
}

static uint64_t now_ms(void)
{
#ifdef RADAR_GATEWAY_HOST_TEST
    return 0U;
#else
    int64_t now_us = esp_timer_get_time();
    return now_us > 0 ? (uint64_t)(now_us / 1000) : 0U;
#endif
}

static size_t slot_index_for_local_id(uint8_t local_id)
{
    return local_id >= 1U && local_id <= RADAR_GATEWAY_MAX_REMOTE_SOURCES
        ? (size_t)(local_id - 1U) : RADAR_GATEWAY_MAX_REMOTE_SOURCES;
}

static bool sequence_after(uint32_t candidate, uint32_t previous)
{
    return candidate != previous && (uint32_t)(candidate - previous) < 0x80000000U;
}

static bool object_has_exact_keys(cJSON *object,
                                  const char *const *keys,
                                  size_t key_count)
{
    if (!cJSON_IsObject(object) || keys == NULL || key_count == 0U || key_count > 32U) {
        return false;
    }
    uint32_t seen = 0U;
    for (cJSON *item = object->child; item != NULL; item = item->next) {
        if (item->string == NULL) return false;
        size_t index = 0U;
        while (index < key_count && strcmp(item->string, keys[index]) != 0) ++index;
        if (index == key_count || (seen & (1UL << index)) != 0U) return false;
        seen |= 1UL << index;
    }
    const uint32_t expected = key_count == 32U ? UINT32_MAX : ((1UL << key_count) - 1UL);
    return seen == expected;
}

static bool json_integer_in_range(cJSON *item, double min, double max, double *out)
{
    if (!cJSON_IsNumber(item) || !isfinite(item->valuedouble) ||
        item->valuedouble < min || item->valuedouble > max ||
        floor(item->valuedouble) != item->valuedouble) {
        return false;
    }
    if (out != NULL) *out = item->valuedouble;
    return true;
}

static radar_gateway_ingest_result_t parse_target(cJSON *item,
                                                  radar_gateway_target_t *out)
{
    static const char *const keys[] = {
        "slot", "x_mm", "y_mm", "speed_cm_s", "resolution_mm", "distance_mm"
    };
    if (out == NULL || !object_has_exact_keys(item, keys, sizeof(keys) / sizeof(keys[0]))) {
        return RADAR_GATEWAY_INGEST_INVALID_TARGETS;
    }
    double slot = 0.0;
    double x = 0.0;
    double y = 0.0;
    double speed = 0.0;
    double resolution = 0.0;
    double distance = 0.0;
    if (!json_integer_in_range(cJSON_GetObjectItemCaseSensitive(item, "slot"),
                               0.0, LD2450_MAX_TARGETS - 1U, &slot) ||
        !json_integer_in_range(cJSON_GetObjectItemCaseSensitive(item, "x_mm"),
                               INT16_MIN, INT16_MAX, &x) ||
        !json_integer_in_range(cJSON_GetObjectItemCaseSensitive(item, "y_mm"),
                               INT16_MIN, INT16_MAX, &y) ||
        !json_integer_in_range(cJSON_GetObjectItemCaseSensitive(item, "speed_cm_s"),
                               INT16_MIN, INT16_MAX, &speed) ||
        !json_integer_in_range(cJSON_GetObjectItemCaseSensitive(item, "resolution_mm"),
                               0.0, UINT16_MAX, &resolution) ||
        !json_integer_in_range(cJSON_GetObjectItemCaseSensitive(item, "distance_mm"),
                               0.0, UINT32_MAX, &distance)) {
        return RADAR_GATEWAY_INGEST_INVALID_TARGETS;
    }
    memset(out, 0, sizeof(*out));
    out->valid = true;
    out->slot = (uint8_t)slot;
    out->x_mm = (int16_t)x;
    out->y_mm = (int16_t)y;
    out->speed_cm_s = (int16_t)speed;
    out->resolution_mm = (uint16_t)resolution;
    out->distance_mm = (uint32_t)distance;
    out->confidence = 0U;
    return RADAR_GATEWAY_INGEST_ACCEPTED;
}

static radar_gateway_ingest_result_t parse_json(const char *json,
                                                size_t json_len,
                                                radar_gateway_sample_t *out)
{
    if (out != NULL) memset(out, 0, sizeof(*out));
    if (json == NULL || out == NULL || json_len == 0U) {
        return RADAR_GATEWAY_INGEST_INVALID_ARGUMENT;
    }
    if (json_len > RADAR_GATEWAY_MAX_BODY_BYTES) {
        return RADAR_GATEWAY_INGEST_TOO_LARGE;
    }

    const char *parse_end = NULL;
    cJSON *root = cJSON_ParseWithLengthOpts(json, json_len, &parse_end, false);
    if (root == NULL) return RADAR_GATEWAY_INGEST_INVALID_JSON;
    while (parse_end != NULL && parse_end < json + json_len &&
           isspace((unsigned char)*parse_end)) ++parse_end;
    if (parse_end == NULL || parse_end != json + json_len) {
        cJSON_Delete(root);
        return RADAR_GATEWAY_INGEST_INVALID_JSON;
    }

    static const char *const root_keys[] = {"p", "id", "t", "u", "q", "v"};
    if (!object_has_exact_keys(root, root_keys, sizeof(root_keys) / sizeof(root_keys[0]))) {
        cJSON_Delete(root);
        return RADAR_GATEWAY_INGEST_INVALID_SCHEMA;
    }
    double number = 0.0;
    if (!json_integer_in_range(cJSON_GetObjectItemCaseSensitive(root, "p"), 2.0, 2.0, &number)) {
        cJSON_Delete(root);
        return RADAR_GATEWAY_INGEST_INVALID_SCHEMA;
    }
    if (!json_integer_in_range(cJSON_GetObjectItemCaseSensitive(root, "id"), 1.0, 2.0, &number)) {
        cJSON_Delete(root);
        return RADAR_GATEWAY_INGEST_INVALID_LOCAL_ID;
    }
    out->local_id = (uint8_t)number;
    cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "t");
    if (!cJSON_IsString(type) || type->valuestring == NULL || strcmp(type->valuestring, "radar") != 0) {
        cJSON_Delete(root);
        return RADAR_GATEWAY_INGEST_INVALID_SCHEMA;
    }
    if (!json_integer_in_range(cJSON_GetObjectItemCaseSensitive(root, "u"),
                               0.0, UINT32_MAX, &number)) {
        cJSON_Delete(root);
        return RADAR_GATEWAY_INGEST_INVALID_SCHEMA;
    }
    out->request_uptime_ms = (uint32_t)number;
    if (!json_integer_in_range(cJSON_GetObjectItemCaseSensitive(root, "q"),
                               1.0, UINT32_MAX, &number)) {
        cJSON_Delete(root);
        return RADAR_GATEWAY_INGEST_INVALID_SEQUENCE;
    }
    out->request_sequence = (uint32_t)number;

    cJSON *value = cJSON_GetObjectItemCaseSensitive(root, "v");
    static const char *const value_keys[] = {
        "link_state", "sample_valid", "frame_seq", "frame_uptime_ms", "target_count", "targets"
    };
    if (!object_has_exact_keys(value, value_keys, sizeof(value_keys) / sizeof(value_keys[0])) ||
        !json_integer_in_range(cJSON_GetObjectItemCaseSensitive(value, "link_state"),
                               0.0, 7.0, &number)) {
        cJSON_Delete(root);
        return RADAR_GATEWAY_INGEST_INVALID_SCHEMA;
    }
    out->link_state = (uint8_t)number;
    if (!json_integer_in_range(cJSON_GetObjectItemCaseSensitive(value, "sample_valid"),
                               0.0, 1.0, &number)) {
        cJSON_Delete(root);
        return RADAR_GATEWAY_INGEST_INVALID_SCHEMA;
    }
    out->sample_valid = number != 0.0;
    if (!json_integer_in_range(cJSON_GetObjectItemCaseSensitive(value, "frame_seq"),
                               0.0, UINT32_MAX, &number)) {
        cJSON_Delete(root);
        return RADAR_GATEWAY_INGEST_INVALID_SEQUENCE;
    }
    out->frame_seq = (uint32_t)number;
    if (!json_integer_in_range(cJSON_GetObjectItemCaseSensitive(value, "frame_uptime_ms"),
                               0.0, UINT32_MAX, &number)) {
        cJSON_Delete(root);
        return RADAR_GATEWAY_INGEST_INVALID_SCHEMA;
    }
    out->frame_uptime_ms = (uint32_t)number;
    if (!json_integer_in_range(cJSON_GetObjectItemCaseSensitive(value, "target_count"),
                               0.0, LD2450_MAX_TARGETS, &number)) {
        cJSON_Delete(root);
        return RADAR_GATEWAY_INGEST_INVALID_TARGETS;
    }
    out->target_count = (uint8_t)number;
    cJSON *targets = cJSON_GetObjectItemCaseSensitive(value, "targets");
    if (!cJSON_IsArray(targets) || cJSON_GetArraySize(targets) != out->target_count) {
        cJSON_Delete(root);
        return RADAR_GATEWAY_INGEST_INVALID_TARGETS;
    }
    for (uint8_t i = 0U; i < out->target_count; ++i) {
        radar_gateway_ingest_result_t ret = parse_target(cJSON_GetArrayItem(targets, i),
                                                         &out->targets[i]);
        if (ret != RADAR_GATEWAY_INGEST_ACCEPTED) {
            cJSON_Delete(root);
            return ret;
        }
        for (uint8_t previous = 0U; previous < i; ++previous) {
            if (out->targets[previous].slot == out->targets[i].slot) {
                cJSON_Delete(root);
                return RADAR_GATEWAY_INGEST_INVALID_TARGETS;
            }
        }
    }
    if ((!out->sample_valid && out->target_count != 0U) ||
        (out->sample_valid && out->frame_seq == 0U)) {
        cJSON_Delete(root);
        return RADAR_GATEWAY_INGEST_INVALID_TARGETS;
    }
    cJSON_Delete(root);
    return RADAR_GATEWAY_INGEST_ACCEPTED;
}

static bool sample_equal(const radar_gateway_sample_t *a,
                         const radar_gateway_sample_t *b)
{
    if (a == NULL || b == NULL || a->local_id != b->local_id ||
        a->link_state != b->link_state || a->sample_valid != b->sample_valid ||
        a->request_uptime_ms != b->request_uptime_ms ||
        a->request_sequence != b->request_sequence || a->frame_seq != b->frame_seq ||
        a->frame_uptime_ms != b->frame_uptime_ms || a->target_count != b->target_count) return false;
    for (size_t i = 0U; i < LD2450_MAX_TARGETS; ++i) {
        const radar_gateway_target_t *x = &a->targets[i];
        const radar_gateway_target_t *y = &b->targets[i];
        if (x->valid != y->valid || x->slot != y->slot || x->x_mm != y->x_mm ||
            x->y_mm != y->y_mm || x->speed_cm_s != y->speed_cm_s ||
            x->resolution_mm != y->resolution_mm || x->distance_mm != y->distance_mm ||
            x->confidence != y->confidence) return false;
    }
    return true;
}

/* Narrow adapter: HTTP schema never reaches coordinate/zone/tracker code directly. */
static void radar_remote_adapter_build_observation(const radar_gateway_sample_t *sample,
                                                   radar_frame_t *frame)
{
    memset(frame, 0, sizeof(*frame));
    frame->frame_seq = sample->frame_seq;
    frame->target_count = sample->target_count;
    for (uint8_t i = 0U; i < sample->target_count; ++i) {
        frame->targets[i] = (radar_target_t){
            .valid = sample->targets[i].valid,
            .x_mm = sample->targets[i].x_mm,
            .y_mm = sample->targets[i].y_mm,
            .speed_cm_s = sample->targets[i].speed_cm_s,
            .resolution_mm = sample->targets[i].resolution_mm,
            .distance_mm = sample->targets[i].distance_mm,
            .confidence = sample->targets[i].confidence,
        };
    }
}

static uint32_t target_distance_sq(int32_t x1, int32_t y1, int32_t x2, int32_t y2)
{
    const int64_t dx = (int64_t)x1 - x2;
    const int64_t dy = (int64_t)y1 - y2;
    const uint64_t distance = (uint64_t)(dx * dx) + (uint64_t)(dy * dy);
    return distance > UINT32_MAX ? UINT32_MAX : (uint32_t)distance;
}

static uint32_t track_id_for_target(const radar_spatial_snapshot_t *snapshot,
                                    const radar_spatial_target_t *target,
                                    bool *visible,
                                    uint8_t *confidence)
{
    uint32_t best_distance = UINT32_MAX;
    uint32_t track_id = 0U;
    bool is_visible = false;
    for (size_t i = 0U; i < RADAR_TRACKER_MAX_TRACKS; ++i) {
        const radar_track_snapshot_t *track = &snapshot->tracks[i];
        if (!track->active) continue;
        const uint32_t distance = target_distance_sq(track->x_mm, track->y_mm,
                                                     target->x_mm, target->y_mm);
        if (distance < best_distance) {
            best_distance = distance;
            track_id = track->track_id;
            is_visible = track->visible;
        }
    }
    if (visible != NULL) *visible = is_visible;
    if (confidence != NULL) {
        *confidence = 0U;
        for (size_t i = 0U; i < RADAR_TRACKER_MAX_TRACKS; ++i) {
            if (snapshot->tracks[i].active && snapshot->tracks[i].track_id == track_id) {
                *confidence = (uint8_t)(snapshot->tracks[i].confidence > 100U
                    ? 100U : snapshot->tracks[i].confidence);
                break;
            }
        }
    }
    return track_id;
}

static void add_zone(radar_gateway_output_t *output,
                     uint8_t zone_id,
                     radar_zone_type_t type)
{
    if (zone_id == 0U || type == RADAR_ZONE_NONE) return;
    for (uint8_t i = 0U; i < output->zone_count; ++i) {
        if (output->zones[i].zone_id == zone_id) {
            if (output->zones[i].target_count < UINT8_MAX) ++output->zones[i].target_count;
            return;
        }
    }
    if (output->zone_count >= RADAR_ZONE_MAP_MAX_ZONES) return;
    output->zones[output->zone_count++] = (radar_gateway_zone_output_t){
        .zone_id = zone_id,
        .type = type,
        .target_count = 1U,
    };
}

static void build_output(radar_gateway_slot_t *slot)
{
    radar_spatial_snapshot_t snapshot;
    radar_spatial_state_get_snapshot(&slot->spatial, &snapshot);
    memset(&slot->output, 0, sizeof(slot->output));
    slot->output.local_id = slot->local_id;
    const char *device_id = radar_registry_device_id(slot->source);
    if (device_id != NULL) {
        strncpy(slot->output.device_id, device_id, sizeof(slot->output.device_id) - 1U);
    }
    slot->output.radar_online = slot->last_sample.link_state == 5U;
    slot->output.occupancy = snapshot.occupancy_state;
    slot->output.motion = snapshot.motion_state;
    slot->output.updated_at_ms = slot->last_received_ms;

    for (uint8_t i = 0U; slot->last_sample.sample_valid && i < snapshot.accepted_target_count &&
                           i < LD2450_MAX_TARGETS; ++i) {
        const radar_spatial_target_t *target = &snapshot.accepted_targets[i];
        bool visible = false;
        uint8_t confidence = 0U;
        uint8_t zone_id = 0U;
        radar_zone_type_t zone_type = RADAR_ZONE_NONE;
        (void)radar_zone_map_resolve(&slot->spatial.zone_map,
                                     target->x_mm,
                                     target->y_mm,
                                     0U,
                                     &zone_id,
                                     &zone_type);
        slot->output.targets[slot->output.target_count++] = (radar_gateway_target_output_t){
            .track_id = track_id_for_target(&snapshot, target, &visible, &confidence),
            .x_mm = target->x_mm,
            .y_mm = target->y_mm,
            .speed_cm_s = target->speed_cm_s,
            .resolution_mm = target->resolution_mm,
            .distance_mm = target->distance_mm,
            .confidence = confidence,
            .zone_id = zone_id,
            .visible = visible,
        };
        add_zone(&slot->output, zone_id, zone_type);
    }
}

static void update_rate_and_publish(radar_gateway_slot_t *slot, uint64_t current_ms)
{
    if (slot == NULL || slot->source >= RADAR_SOURCE_COUNT) {
        return;
    }
    radar_spatial_snapshot_t snapshot;
    radar_spatial_state_get_snapshot(&slot->spatial, &snapshot);
    uint8_t retained_track_count = 0U;
    for (size_t i = 0U; i < RADAR_TRACKER_MAX_TRACKS; ++i) {
        if (snapshot.tracks[i].active && snapshot.tracks[i].track_id != 0U &&
            retained_track_count < UINT8_MAX) {
            ++retained_track_count;
        }
    }
    (void)radar_rate_manager_update(&slot->rate_manager,
                                    snapshot.accepted_target_count,
                                    snapshot.active_track_count,
                                    retained_track_count,
                                    current_ms);
#ifndef RADAR_GATEWAY_HOST_TEST
    radar_log_manager_publish(slot->source, &snapshot, &slot->rate_manager);
#endif
}

static radar_presence_state_t legacy_state(const radar_gateway_output_t *output)
{
    if (output == NULL || !output->radar_online) return RADAR_STATE_UNKNOWN;
    switch (output->occupancy) {
    case RADAR_OCCUPANCY_PRESENT:
        return output->motion == RADAR_MOTION_MOVING ? RADAR_STATE_MOTION : RADAR_STATE_PRESENT;
    case RADAR_OCCUPANCY_HOLD:
        return RADAR_STATE_HOLD;
    case RADAR_OCCUPANCY_VACANT_INFERRED:
        return RADAR_STATE_VACANT_INFERRED;
    case RADAR_OCCUPANCY_UNKNOWN:
    default:
        return RADAR_STATE_UNKNOWN;
    }
}

static radar_registry_update_result_t update_registry(radar_gateway_slot_t *slot)
{
    radar_spatial_snapshot_t spatial;
    radar_spatial_state_get_snapshot(&slot->spatial, &spatial);
    radar_protocol_payload_t payload;
    memset(&payload, 0, sizeof(payload));
    payload.schema_version = RADAR_PROTOCOL_SCHEMA_VERSION;
    payload.local_id = slot->local_id;
    payload.sequence = slot->last_sample.request_sequence;
    payload.uptime_ms = slot->last_sample.request_uptime_ms;
    payload.state = legacy_state(&slot->output);
    payload.target_count = slot->last_sample.sample_valid ? slot->output.target_count : 0U;
    payload.uart_online = slot->last_sample.link_state == 5U;
    payload.frame_fresh = spatial.sensor_state == RADAR_SENSOR_VALID;
    payload.last_motion_age_ms = slot->output.motion == RADAR_MOTION_MOVING ? 0U : UINT32_MAX;
    for (uint8_t i = 0U; i < payload.target_count; ++i) {
        const radar_gateway_target_output_t *target = &slot->output.targets[i];
        payload.targets[i] = (radar_target_t){
            .valid = true,
            .x_mm = target->x_mm > INT16_MAX ? INT16_MAX :
                    (target->x_mm < INT16_MIN ? INT16_MIN : (int16_t)target->x_mm),
            .y_mm = target->y_mm > INT16_MAX ? INT16_MAX :
                    (target->y_mm < INT16_MIN ? INT16_MIN : (int16_t)target->y_mm),
            .speed_cm_s = target->speed_cm_s,
            .resolution_mm = target->resolution_mm,
            .distance_mm = target->distance_mm,
            .confidence = target->confidence,
        };
    }
    bool state_changed = false;
    return radar_registry_update_remote(slot->source,
                                        &payload,
                                        slot->session_generation,
                                        slot->last_received_ms,
                                        &state_changed);
}

static bool resolve_identity(uint8_t local_id,
                             const char *device_id,
                             uint32_t *out_generation)
{
    if (device_id == NULL || device_id[0] == '\0' || out_generation == NULL) return false;
    radar_source_id_t source = radar_registry_source_for_local_id(local_id);
    const char *expected_device_id = radar_registry_device_id(source);
    if (source == RADAR_SOURCE_COUNT || expected_device_id == NULL ||
        strcmp(expected_device_id, device_id) != 0) {
        return false;
    }
    *out_generation = 1U;
    return true;
}

radar_gateway_ingest_result_t radar_gateway_ingest_admit(
    const radar_gateway_sample_t *sample,
    uint32_t session_generation,
    uint64_t received_at_ms,
    radar_gateway_output_t *out)
{
    if (sample == NULL || received_at_ms == 0U || session_generation == 0U) {
        return RADAR_GATEWAY_INGEST_INVALID_ARGUMENT;
    }
    const size_t index = slot_index_for_local_id(sample->local_id);
    if (index >= RADAR_GATEWAY_MAX_REMOTE_SOURCES || sample->request_sequence == 0U ||
        sample->target_count > LD2450_MAX_TARGETS ||
        (!sample->sample_valid && sample->target_count != 0U)) {
        return RADAR_GATEWAY_INGEST_INVALID_LOCAL_ID;
    }
    if (!s_initialized || !gateway_lock()) return RADAR_GATEWAY_INGEST_UNAVAILABLE;
    radar_gateway_slot_t *slot = &s_slots[index];
    if (slot->session_generation != session_generation) {
        slot->session_generation = session_generation;
        slot->has_sample = false;
        slot->last_sequence = 0U;
        slot->last_received_ms = 0U;
        radar_spatial_state_init(&slot->spatial, NULL, received_at_ms);
        radar_rate_manager_init(&slot->rate_manager, received_at_ms);
    }
    if (slot->has_sample && sample->request_sequence == slot->last_sequence) {
        const bool equal = sample_equal(sample, &slot->last_sample);
        if (equal && out != NULL) *out = slot->output;
        gateway_unlock();
        return equal ? RADAR_GATEWAY_INGEST_DUPLICATE : RADAR_GATEWAY_INGEST_SEQUENCE_CONFLICT;
    }
    if (slot->has_sample && !sequence_after(sample->request_sequence, slot->last_sequence)) {
        const bool reboot = sample->request_uptime_ms + 1000U <
                            slot->last_sample.request_uptime_ms;
        if (!reboot) {
            gateway_unlock();
            return RADAR_GATEWAY_INGEST_SEQUENCE_BACKWARD;
        }
        slot->has_sample = false;
        radar_spatial_state_init(&slot->spatial, NULL, received_at_ms);
        radar_rate_manager_init(&slot->rate_manager, received_at_ms);
    }

    if (sample->sample_valid && slot->has_sample && slot->last_sample.sample_valid &&
        !sequence_after(sample->frame_seq, slot->last_sample.frame_seq)) {
        gateway_unlock();
        return RADAR_GATEWAY_INGEST_SEQUENCE_BACKWARD;
    }

    slot->local_id = sample->local_id;
    slot->source = radar_registry_source_for_local_id(sample->local_id);
    slot->last_sample = *sample;
    slot->last_sequence = sample->request_sequence;
    slot->last_received_ms = received_at_ms;
    slot->has_sample = true;
    if (sample->sample_valid) {
        radar_frame_t frame;
        radar_remote_adapter_build_observation(sample, &frame);
        frame.received_at_ms = received_at_ms;
        radar_spatial_state_on_frame(&slot->spatial, &frame, true, received_at_ms);
        radar_spatial_state_poll(&slot->spatial, RADAR_UART_RECOVERY_VALID, received_at_ms);
    } else {
        radar_spatial_state_poll(&slot->spatial,
                                 sample->link_state == 5U ? RADAR_UART_RECOVERY_WAITING_VALID :
                                                            RADAR_UART_RECOVERY_BACKOFF,
                                 received_at_ms);
    }
    update_rate_and_publish(slot, received_at_ms);
    build_output(slot);
    const radar_registry_update_result_t registry_ret = update_registry(slot);
    if (registry_ret != RADAR_REGISTRY_UPDATE_ACCEPTED &&
        registry_ret != RADAR_REGISTRY_UPDATE_DUPLICATE) {
        gateway_unlock();
        return RADAR_GATEWAY_INGEST_UNAVAILABLE;
    }
    if (out != NULL) *out = slot->output;
    if (slot->last_log_ms == 0U || received_at_ms - slot->last_log_ms >= 1000U) {
        slot->last_log_ms = received_at_ms;
        ESP_LOGI(TAG,
                 "radar_ingest_ok device_id=%s targets=%u tracking_state=%s presence_state=%s",
                 slot->output.device_id,
                 (unsigned int)slot->output.target_count,
                 radar_gateway_tracking_name(&slot->output),
                 radar_gateway_occupancy_name(slot->output.occupancy));
    }
    gateway_unlock();
    return RADAR_GATEWAY_INGEST_ACCEPTED;
}

radar_gateway_ingest_result_t radar_gateway_ingest_process_json(
    const char *json,
    size_t json_len,
    const char *device_id,
    uint64_t received_at_ms,
    radar_gateway_sample_t *out_sample,
    radar_gateway_output_t *out)
{
    radar_gateway_sample_t sample;
    const radar_gateway_ingest_result_t parse_ret = parse_json(json, json_len, &sample);
    if (parse_ret != RADAR_GATEWAY_INGEST_ACCEPTED) return parse_ret;
    if (out_sample != NULL) *out_sample = sample;

    uint32_t session_generation = 0U;
    if (!resolve_identity(sample.local_id, device_id, &session_generation)) {
        radar_registry_note_identity_mismatch(radar_registry_source_for_local_id(sample.local_id));
        return RADAR_GATEWAY_INGEST_IDENTITY_MISMATCH;
    }
    return radar_gateway_ingest_admit(&sample, session_generation, received_at_ms, out);
}

bool radar_gateway_ingest_get_output(uint8_t local_id, radar_gateway_output_t *out)
{
    const size_t index = slot_index_for_local_id(local_id);
    if (out == NULL || index >= RADAR_GATEWAY_MAX_REMOTE_SOURCES ||
        !s_initialized || !gateway_lock()) return false;
    *out = s_slots[index].output;
    gateway_unlock();
    return s_slots[index].has_sample;
}

void radar_gateway_ingest_poll(uint64_t current_ms)
{
    if (!s_initialized || current_ms == 0U || !gateway_lock()) return;
    for (size_t i = 0U; i < RADAR_GATEWAY_MAX_REMOTE_SOURCES; ++i) {
        radar_gateway_slot_t *slot = &s_slots[i];
        if (!slot->has_sample || current_ms < slot->last_received_ms) continue;
        const radar_uart_recovery_state_t state =
            current_ms - slot->last_received_ms > RADAR_GATEWAY_OFFLINE_TIMEOUT_MS
                ? RADAR_UART_RECOVERY_OFFLINE : RADAR_UART_RECOVERY_VALID;
        radar_spatial_state_poll(&slot->spatial, state, current_ms);
        update_rate_and_publish(slot, current_ms);
        build_output(slot);
    }
    gateway_unlock();
}

esp_err_t radar_gateway_ingest_start(void)
{
    if (s_initialized) return ESP_OK;
#ifndef RADAR_GATEWAY_HOST_TEST
    s_lock = xSemaphoreCreateMutexStatic(&s_lock_storage);
    if (s_lock == NULL || !radar_registry_init()) return ESP_ERR_NO_MEM;
#else
    if (!radar_registry_init()) return ESP_ERR_NO_MEM;
#endif
    memset(s_slots, 0, sizeof(s_slots));
    for (size_t i = 0U; i < RADAR_GATEWAY_MAX_REMOTE_SOURCES; ++i) {
        s_slots[i].local_id = (uint8_t)(i + 1U);
        s_slots[i].source = radar_registry_source_for_local_id((uint8_t)(i + 1U));
        radar_spatial_state_init(&s_slots[i].spatial, NULL, now_ms());
        radar_rate_manager_init(&s_slots[i].rate_manager, now_ms());
    }
    s_initialized = true;
    ESP_LOGI(TAG, "radar_gateway_ingest ready sources=%u", RADAR_GATEWAY_MAX_REMOTE_SOURCES);
    return ESP_OK;
}

const char *radar_gateway_occupancy_name(radar_occupancy_state_t state)
{
    switch (state) {
    case RADAR_OCCUPANCY_PRESENT: return "present";
    case RADAR_OCCUPANCY_HOLD: return "hold";
    case RADAR_OCCUPANCY_VACANT_INFERRED: return "vacant";
    case RADAR_OCCUPANCY_UNKNOWN:
    default: return "unknown";
    }
}

const char *radar_gateway_motion_name(radar_motion_state_t state)
{
    switch (state) {
    case RADAR_MOTION_MOVING: return "moving";
    case RADAR_MOTION_STILL_CANDIDATE: return "still_candidate";
    case RADAR_MOTION_NONE: return "none";
    case RADAR_MOTION_UNKNOWN:
    default: return "unknown";
    }
}

const char *radar_gateway_tracking_name(const radar_gateway_output_t *output)
{
    if (output == NULL || output->target_count == 0U) return "none";
    for (uint8_t i = 0U; i < output->target_count; ++i) {
        if (output->targets[i].visible) return "visible";
    }
    return "hold";
}

const char *radar_gateway_ingest_result_name(radar_gateway_ingest_result_t result)
{
    switch (result) {
    case RADAR_GATEWAY_INGEST_ACCEPTED: return "accepted";
    case RADAR_GATEWAY_INGEST_DUPLICATE: return "duplicate";
    case RADAR_GATEWAY_INGEST_INVALID_ARGUMENT: return "invalid_argument";
    case RADAR_GATEWAY_INGEST_TOO_LARGE: return "too_large";
    case RADAR_GATEWAY_INGEST_INVALID_JSON: return "invalid_json";
    case RADAR_GATEWAY_INGEST_INVALID_SCHEMA: return "invalid_schema";
    case RADAR_GATEWAY_INGEST_INVALID_LOCAL_ID: return "invalid_local_id";
    case RADAR_GATEWAY_INGEST_INVALID_SEQUENCE: return "invalid_sequence";
    case RADAR_GATEWAY_INGEST_INVALID_TARGETS: return "invalid_targets";
    case RADAR_GATEWAY_INGEST_IDENTITY_MISMATCH: return "identity_mismatch";
    case RADAR_GATEWAY_INGEST_SEQUENCE_CONFLICT: return "sequence_conflict";
    case RADAR_GATEWAY_INGEST_SEQUENCE_BACKWARD: return "sequence_backward";
    case RADAR_GATEWAY_INGEST_UNAVAILABLE: return "unavailable";
    default: return "unknown";
    }
}
