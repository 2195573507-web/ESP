#include "radar_registry.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

#ifndef RADAR_DOMAIN_HOST_TEST
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#endif

#define RADAR_REBOOT_ROLLBACK_MIN_MS 1000U

/*
 * 注册表是三路雷达的并发读取快照。远端更新按会话代次、序号和上报运行时间
 * 排序：同序号仅接受内容完全一致的重传，运行时间明显回退才视为设备重启。
 */

typedef struct {
    radar_registry_entry_t entry;
    radar_protocol_payload_t last_payload;
    bool has_payload;
} radar_registry_slot_t;

static radar_registry_slot_t s_slots[RADAR_SOURCE_COUNT];
static bool s_initialized;
static uint32_t s_unattributed_parse_errors;
static radar_home_presence_t s_home_presence;

#ifndef RADAR_DOMAIN_HOST_TEST
static StaticSemaphore_t s_lock_storage;
static SemaphoreHandle_t s_lock;
#endif

static const char *const s_source_names[RADAR_SOURCE_COUNT] = {
    "S3_LOCAL",
    "C51",
    "C52",
};

static const char *const s_device_ids[RADAR_SOURCE_COUNT] = {
    "sensair_s3_gateway_01",
    "sensair_shuttle_01",
    "sensair_shuttle_02",
};

static const char *const s_room_ids[RADAR_SOURCE_COUNT] = {
    "s3_local",
    "living_room",
    "bedroom",
};

static bool source_valid(radar_source_id_t source)
{
    return source >= RADAR_SOURCE_S3_LOCAL && source < RADAR_SOURCE_COUNT;
}

static void sat_inc_u32(uint32_t *value)
{
    if (value != NULL && *value < UINT32_MAX) {
        ++(*value);
    }
}

static uint32_t sat_add_u32(uint32_t a, uint32_t b)
{
    return UINT32_MAX - a < b ? UINT32_MAX : a + b;
}

static void copy_text(char *out, size_t out_size, const char *value)
{
    if (out == NULL || out_size == 0U) {
        return;
    }
    if (value == NULL) {
        out[0] = '\0';
        return;
    }
    size_t length = strlen(value);
    if (length >= out_size) {
        length = out_size - 1U;
    }
    memcpy(out, value, length);
    out[length] = '\0';
}

static const char *presence_state_name(radar_presence_state_t state)
{
    switch (state) {
    case RADAR_STATE_MOTION: return "moving";
    case RADAR_STATE_PRESENT: return "present";
    case RADAR_STATE_HOLD: return "hold";
    case RADAR_STATE_VACANT_INFERRED: return "vacant";
    case RADAR_STATE_UNKNOWN:
    default: return "unknown";
    }
}

static bool state_occupies_room(radar_presence_state_t state)
{
    return state == RADAR_STATE_MOTION || state == RADAR_STATE_PRESENT ||
           state == RADAR_STATE_HOLD;
}

static void refresh_home_presence_locked(radar_source_id_t changed_source,
                                         radar_presence_state_t previous_state,
                                         uint64_t now_ms)
{
    uint8_t occupied_room_count = 0U;
    radar_source_id_t active_source = RADAR_SOURCE_COUNT;
    uint64_t active_transition_ms = 0U;
    for (radar_source_id_t source = RADAR_SOURCE_S3_LOCAL;
         source < RADAR_SOURCE_COUNT;
         source = (radar_source_id_t)(source + 1)) {
        const radar_registry_entry_t *entry = &s_slots[source].entry;
        if (!entry->source_online || !state_occupies_room(entry->snapshot.state)) {
            continue;
        }
        if (occupied_room_count < UINT8_MAX) {
            ++occupied_room_count;
        }
        if (active_source == RADAR_SOURCE_COUNT ||
            entry->last_state_change_ms >= active_transition_ms) {
            active_source = source;
            active_transition_ms = entry->last_state_change_ms;
        }
    }
    s_home_presence.occupied_room_count = occupied_room_count;
    s_home_presence.active_source = active_source;
    copy_text(s_home_presence.active_room, sizeof(s_home_presence.active_room),
              active_source < RADAR_SOURCE_COUNT ? s_room_ids[active_source] : "");
    if (changed_source < RADAR_SOURCE_COUNT &&
        previous_state != s_slots[changed_source].entry.snapshot.state) {
        (void)snprintf(s_home_presence.last_transition, sizeof(s_home_presence.last_transition),
                       "%s:%s->%s", s_room_ids[changed_source],
                       presence_state_name(previous_state),
                       presence_state_name(s_slots[changed_source].entry.snapshot.state));
        s_home_presence.last_transition_ms = now_ms;
    }
}

static bool registry_lock(void)
{
#ifdef RADAR_DOMAIN_HOST_TEST
    return true;
#else
    return s_lock != NULL && xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE;
#endif
}

static void registry_unlock(void)
{
#ifndef RADAR_DOMAIN_HOST_TEST
    xSemaphoreGive(s_lock);
#endif
}

static void initialize_slot(radar_source_id_t source)
{
    radar_registry_slot_t *slot = &s_slots[source];
    memset(slot, 0, sizeof(*slot));
    slot->entry.source = source;
    slot->entry.snapshot.state = RADAR_STATE_UNKNOWN;
    slot->entry.count_summary.count_state = RADAR_PERSON_COUNT_UNKNOWN;
    copy_text(slot->entry.device_id, sizeof(slot->entry.device_id), s_device_ids[source]);
    copy_text(slot->entry.room_id, sizeof(slot->entry.room_id), s_room_ids[source]);
}

bool radar_registry_init(void)
{
    if (s_initialized) {
        return true;
    }
#ifndef RADAR_DOMAIN_HOST_TEST
    s_lock = xSemaphoreCreateMutexStatic(&s_lock_storage);
    if (s_lock == NULL) {
        return false;
    }
#endif
    for (radar_source_id_t source = RADAR_SOURCE_S3_LOCAL;
         source < RADAR_SOURCE_COUNT;
         source = (radar_source_id_t)(source + 1)) {
        initialize_slot(source);
    }
    s_unattributed_parse_errors = 0U;
    memset(&s_home_presence, 0, sizeof(s_home_presence));
    s_home_presence.active_source = RADAR_SOURCE_COUNT;
    s_initialized = true;
    return true;
}

radar_source_id_t radar_registry_source_for_local_id(uint8_t local_id)
{
    if (local_id == 1U) {
        return RADAR_SOURCE_C51;
    }
    if (local_id == 2U) {
        return RADAR_SOURCE_C52;
    }
    return RADAR_SOURCE_COUNT;
}

const char *radar_registry_source_name(radar_source_id_t source)
{
    return source_valid(source) ? s_source_names[source] : "UNKNOWN";
}

const char *radar_registry_device_id(radar_source_id_t source)
{
    return source_valid(source) ? s_device_ids[source] : NULL;
}

const char *radar_registry_room_id(radar_source_id_t source)
{
    return source_valid(source) ? s_room_ids[source] : NULL;
}

bool radar_registry_get(radar_source_id_t source, radar_registry_entry_t *out)
{
    if (!s_initialized || !source_valid(source) || out == NULL || !registry_lock()) {
        return false;
    }
    *out = s_slots[source].entry;
    registry_unlock();
    return true;
}

size_t radar_registry_snapshot(radar_registry_entry_t *out, size_t capacity)
{
    if (!s_initialized || out == NULL || capacity == 0U || !registry_lock()) {
        return 0U;
    }
    size_t count = capacity < RADAR_SOURCE_COUNT ? capacity : RADAR_SOURCE_COUNT;
    for (size_t i = 0U; i < count; ++i) {
        out[i] = s_slots[i].entry;
    }
    registry_unlock();
    return count;
}

void radar_registry_get_home_presence(radar_home_presence_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->active_source = RADAR_SOURCE_COUNT;
    if (!s_initialized || !registry_lock()) {
        return;
    }
    *out = s_home_presence;
    registry_unlock();
}

static void copy_payload_to_snapshot(radar_registry_entry_t *entry,
                                     const radar_protocol_payload_t *payload,
                                     uint64_t received_at_ms)
{
    const radar_presence_state_t previous_state = entry->snapshot.state;
    entry->snapshot.state = payload->state;
    entry->snapshot.current_target_count = payload->target_count;
    entry->snapshot.uart_online = payload->uart_online;
    entry->snapshot.frame_fresh = payload->frame_fresh;
    memset(entry->snapshot.targets, 0, sizeof(entry->snapshot.targets));
    memcpy(entry->snapshot.targets, payload->targets, sizeof(payload->targets));
    if (payload->frame_fresh) {
        entry->snapshot.last_valid_frame_ms = received_at_ms;
    }
    if (payload->last_motion_age_ms != UINT32_MAX &&
        received_at_ms >= payload->last_motion_age_ms) {
        entry->snapshot.last_motion_ms = received_at_ms - payload->last_motion_age_ms;
    }
    if (entry->snapshot.state != previous_state) {
        sat_inc_u32(&entry->snapshot.state_seq);
        entry->snapshot.state_since_ms = received_at_ms;
        entry->last_state_change_ms = received_at_ms;
    }
}

static radar_count_summary_t fallback_count_summary(const radar_protocol_payload_t *payload)
{
    radar_count_summary_t summary = {.count_state = RADAR_PERSON_COUNT_UNKNOWN};
    if (payload != NULL) {
        summary.visible_track_count = payload->target_count;
    }
    return summary;
}

radar_registry_update_result_t radar_registry_update_remote(
    radar_source_id_t source,
    const radar_protocol_payload_t *payload,
    const radar_count_summary_t *count_summary,
    uint32_t session_generation,
    uint64_t received_at_ms,
    bool *out_state_changed)
{
    if (out_state_changed != NULL) {
        *out_state_changed = false;
    }
    if (!s_initialized || !source_valid(source) || source == RADAR_SOURCE_S3_LOCAL ||
        payload == NULL || session_generation == 0U || received_at_ms == 0U ||
        radar_registry_source_for_local_id(payload->local_id) != source ||
        !registry_lock()) {
        return RADAR_REGISTRY_UPDATE_INVALID_ARGUMENT;
    }

    radar_registry_slot_t *slot = &s_slots[source];
    radar_registry_entry_t *entry = &slot->entry;
    const bool old_online = entry->source_online;
    const radar_presence_state_t old_state = entry->snapshot.state;

    if (entry->session_generation != session_generation) {
        entry->session_generation = session_generation;
        entry->sequence = 0U;
        entry->source_uptime_ms = 0U;
        slot->has_payload = false;
    }

    if (slot->has_payload && payload->sequence == entry->sequence) {
        if (radar_protocol_payload_equal(payload, &slot->last_payload)) {
            entry->last_report_ms = received_at_ms;
            sat_inc_u32(&entry->diagnostics.duplicate_count);
            registry_unlock();
            return RADAR_REGISTRY_UPDATE_DUPLICATE;
        }
        sat_inc_u32(&entry->diagnostics.sequence_reject_count);
        registry_unlock();
        return RADAR_REGISTRY_UPDATE_SEQUENCE_CONFLICT;
    }

    if (slot->has_payload && payload->sequence < entry->sequence) {
        const bool clear_uptime_rollback =
            payload->uptime_ms + RADAR_REBOOT_ROLLBACK_MIN_MS < entry->source_uptime_ms;
        if (!clear_uptime_rollback) {
            sat_inc_u32(&entry->diagnostics.sequence_reject_count);
            registry_unlock();
            return RADAR_REGISTRY_UPDATE_SEQUENCE_BACKWARD;
        }
        sat_inc_u32(&entry->diagnostics.reboot_reset_count);
    }

    copy_payload_to_snapshot(entry, payload, received_at_ms);
    entry->count_summary = count_summary != NULL ? *count_summary : fallback_count_summary(payload);
    entry->source_online = payload->uart_online && payload->frame_fresh;
    entry->sequence = payload->sequence;
    entry->source_uptime_ms = payload->uptime_ms;
    entry->last_report_ms = received_at_ms;
    slot->last_payload = *payload;
    slot->has_payload = true;
    sat_inc_u32(&entry->diagnostics.accepted_count);

    const bool changed = old_online != entry->source_online || old_state != entry->snapshot.state;
    refresh_home_presence_locked(source, old_state, received_at_ms);
    if (out_state_changed != NULL) {
        *out_state_changed = changed;
    }
    registry_unlock();
    return RADAR_REGISTRY_UPDATE_ACCEPTED;
}

bool radar_registry_update_local(const radar_snapshot_t *snapshot,
                                 const radar_count_summary_t *count_summary,
                                 const radar_registry_local_diagnostics_t *service_diagnostics,
                                 uint64_t received_at_ms,
                                 bool *out_state_changed)
{
    if (out_state_changed != NULL) {
        *out_state_changed = false;
    }
    if (!s_initialized || snapshot == NULL || received_at_ms == 0U || !registry_lock()) {
        return false;
    }

    radar_registry_entry_t *entry = &s_slots[RADAR_SOURCE_S3_LOCAL].entry;
    const bool old_online = entry->source_online;
    const radar_presence_state_t old_state = entry->snapshot.state;
    entry->snapshot = *snapshot;
    entry->count_summary = count_summary != NULL ? *count_summary :
        (radar_count_summary_t){.count_state = RADAR_PERSON_COUNT_UNKNOWN};
    entry->source_online = snapshot->uart_online && snapshot->frame_fresh;
    entry->source_uptime_ms = received_at_ms;
    entry->last_report_ms = received_at_ms;
    entry->last_state_change_ms = old_state != snapshot->state ? received_at_ms :
                                                              entry->last_state_change_ms;
    sat_inc_u32(&entry->diagnostics.accepted_count);
    if (service_diagnostics != NULL) {
        const uint32_t bad_frames =
            sat_add_u32(service_diagnostics->parser_bad_header,
                        service_diagnostics->parser_bad_length);
        const uint32_t parse_errors =
            sat_add_u32(sat_add_u32(bad_frames, service_diagnostics->parser_bad_tail),
                        service_diagnostics->parser_skipped_bytes);
        entry->diagnostics.parse_error_count =
            sat_add_u32(parse_errors, service_diagnostics->uart_read_driver_error);
    }

    const bool changed = old_online != entry->source_online || old_state != snapshot->state;
    refresh_home_presence_locked(RADAR_SOURCE_S3_LOCAL, old_state, received_at_ms);
    if (out_state_changed != NULL) {
        *out_state_changed = changed;
    }
    registry_unlock();
    return true;
}

void radar_registry_note_parse_error(radar_source_id_t source)
{
    if (!s_initialized || !registry_lock()) {
        return;
    }
    if (source_valid(source)) {
        sat_inc_u32(&s_slots[source].entry.diagnostics.parse_error_count);
    } else {
        sat_inc_u32(&s_unattributed_parse_errors);
    }
    registry_unlock();
}

void radar_registry_note_identity_mismatch(radar_source_id_t source)
{
    if (!s_initialized || !source_valid(source) || !registry_lock()) {
        return;
    }
    sat_inc_u32(&s_slots[source].entry.diagnostics.identity_mismatch_count);
    registry_unlock();
}

void radar_registry_refresh(uint64_t now_ms)
{
    if (!s_initialized || now_ms == 0U || !registry_lock()) {
        return;
    }
    for (size_t i = 0U; i < RADAR_SOURCE_COUNT; ++i) {
        radar_registry_entry_t *entry = &s_slots[i].entry;
        if (entry->last_report_ms == 0U || now_ms < entry->last_report_ms ||
            now_ms - entry->last_report_ms <= RADAR_REGISTRY_FRESHNESS_TIMEOUT_MS) {
            continue;
        }
        if (entry->source_online || entry->snapshot.state != RADAR_STATE_UNKNOWN ||
            entry->snapshot.frame_fresh) {
            const radar_presence_state_t previous_state = entry->snapshot.state;
            entry->source_online = false;
            entry->snapshot.state = RADAR_STATE_UNKNOWN;
            entry->snapshot.frame_fresh = false;
            entry->snapshot.current_target_count = 0U;
            memset(entry->snapshot.targets, 0, sizeof(entry->snapshot.targets));
            memset(&entry->count_summary, 0, sizeof(entry->count_summary));
            entry->count_summary.count_state = RADAR_PERSON_COUNT_UNKNOWN;
            sat_inc_u32(&entry->snapshot.state_seq);
            entry->snapshot.state_since_ms = now_ms;
            entry->last_state_change_ms = now_ms;
            sat_inc_u32(&entry->diagnostics.freshness_expiry_count);
            refresh_home_presence_locked((radar_source_id_t)i, previous_state, now_ms);
        }
    }
    registry_unlock();
}

uint32_t radar_registry_unattributed_parse_errors(void)
{
    if (!s_initialized || !registry_lock()) {
        return 0U;
    }
    uint32_t value = s_unattributed_parse_errors;
    registry_unlock();
    return value;
}
