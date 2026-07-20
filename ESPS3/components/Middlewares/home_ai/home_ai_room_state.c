#include "home_ai_room_state.h"

#include <string.h>

#include "esp111_protocol_common.h"

#ifndef HOME_AI_ROOM_STATE_HOST_TEST
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#endif

#define HOME_AI_ROOM_OCCUPIED_CONFIRM_DEFAULT_MS 1500U
#define HOME_AI_ROOM_VACANT_CONFIRM_DEFAULT_MS 60000U
#define HOME_AI_ROOM_MULTIPLE_CONFIRM_DEFAULT_MS 3000U
#define HOME_AI_ROOM_SINGLE_CONFIRM_DEFAULT_MS 10000U
#define HOME_AI_ROOM_QUIET_START_DEFAULT_MINUTE (23U * 60U)
#define HOME_AI_ROOM_QUIET_END_DEFAULT_MINUTE (7U * 60U)

#define HOME_AI_ROOM_OCCUPIED_CONFIRM_MAX_MS 600000U
#define HOME_AI_ROOM_VACANT_CONFIRM_MAX_MS 3600000U
#define HOME_AI_ROOM_COUNT_CONFIRM_MAX_MS 600000U

typedef struct {
    home_ai_room_state_config_t config;
    home_ai_room_state_t state;
    home_ai_room_presence_state_t presence_candidate;
    uint64_t presence_candidate_since_ms;
    home_ai_room_occupancy_mode_t count_candidate_mode;
    uint8_t count_candidate;
    uint64_t count_candidate_since_ms;
} home_ai_room_state_slot_t;

static home_ai_room_state_slot_t s_slots[HOME_AI_ROOM_STATE_COUNT];
static bool s_initialized;

#ifndef HOME_AI_ROOM_STATE_HOST_TEST
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
#define ROOM_STATE_LOCK() portENTER_CRITICAL(&s_lock)
#define ROOM_STATE_UNLOCK() portEXIT_CRITICAL(&s_lock)
#else
#define ROOM_STATE_LOCK() ((void)0)
#define ROOM_STATE_UNLOCK() ((void)0)
#endif

static bool source_valid(radar_source_id_t source)
{
    return source >= RADAR_SOURCE_S3_LOCAL && source < RADAR_SOURCE_COUNT;
}

static bool text_valid(const char *value, size_t capacity)
{
    if (value == NULL || capacity < 2U || value[0] == '\0') {
        return false;
    }
    for (size_t index = 0U; index < capacity; ++index) {
        if (value[index] == '\0') {
            return true;
        }
    }
    return false;
}

static bool voice_terminal_valid(const char *value)
{
    if (value == NULL) {
        return false;
    }
    if (value[0] == '\0') {
        return true;
    }
    return text_valid(value, HOME_AI_ROOM_STATE_VOICE_TERMINAL_ID_LEN) &&
           (strcmp(value, ESP111_PROTOCOL_TERMINAL_DEVICE_ID_C51) == 0 ||
            strcmp(value, ESP111_PROTOCOL_TERMINAL_DEVICE_ID_C52) == 0);
}

static void copy_text(char *out, size_t out_size, const char *value)
{
    if (out == NULL || out_size == 0U) {
        return;
    }
    size_t length = 0U;
    if (value != NULL) {
        while (length + 1U < out_size && value[length] != '\0') {
            ++length;
        }
        memcpy(out, value, length);
    }
    out[length] = '\0';
}

static bool elapsed(uint64_t now_ms, uint64_t since_ms, uint32_t required_ms)
{
    return now_ms >= since_ms && now_ms - since_ms >= required_ms;
}

static void set_default_config(home_ai_room_state_config_t *config,
                               radar_source_id_t source,
                               const char *room_id,
                               const char *voice_terminal_device_id)
{
    memset(config, 0, sizeof(*config));
    config->source = source;
    copy_text(config->room_id, sizeof(config->room_id), room_id);
    copy_text(config->voice_terminal_device_id,
              sizeof(config->voice_terminal_device_id),
              voice_terminal_device_id);
    config->occupied_confirm_ms = HOME_AI_ROOM_OCCUPIED_CONFIRM_DEFAULT_MS;
    config->vacant_confirm_ms = HOME_AI_ROOM_VACANT_CONFIRM_DEFAULT_MS;
    config->multiple_confirm_ms = HOME_AI_ROOM_MULTIPLE_CONFIRM_DEFAULT_MS;
    config->single_confirm_ms = HOME_AI_ROOM_SINGLE_CONFIRM_DEFAULT_MS;
    config->quiet_start_minute = HOME_AI_ROOM_QUIET_START_DEFAULT_MINUTE;
    config->quiet_end_minute = HOME_AI_ROOM_QUIET_END_DEFAULT_MINUTE;
}

static bool config_valid(const home_ai_room_state_config_t *config)
{
    return config != NULL && source_valid(config->source) &&
           text_valid(config->room_id, sizeof(config->room_id)) &&
           voice_terminal_valid(config->voice_terminal_device_id) &&
           config->occupied_confirm_ms > 0U &&
           config->occupied_confirm_ms <= HOME_AI_ROOM_OCCUPIED_CONFIRM_MAX_MS &&
           config->vacant_confirm_ms > 0U &&
           config->vacant_confirm_ms <= HOME_AI_ROOM_VACANT_CONFIRM_MAX_MS &&
           config->multiple_confirm_ms > 0U &&
           config->multiple_confirm_ms <= HOME_AI_ROOM_COUNT_CONFIRM_MAX_MS &&
           config->single_confirm_ms > 0U &&
           config->single_confirm_ms <= HOME_AI_ROOM_COUNT_CONFIRM_MAX_MS &&
           config->quiet_start_minute < 1440U && config->quiet_end_minute < 1440U &&
           config->quiet_start_minute != config->quiet_end_minute;
}

static void reset_slot_locked(home_ai_room_state_slot_t *slot,
                              const home_ai_room_state_config_t *config)
{
    memset(slot, 0, sizeof(*slot));
    slot->config = *config;
    slot->state.source = config->source;
    copy_text(slot->state.room_id, sizeof(slot->state.room_id), config->room_id);
    slot->state.presence_state = HOME_AI_ROOM_PRESENCE_UNKNOWN;
    slot->state.occupancy_mode = HOME_AI_ROOM_OCCUPANCY_UNKNOWN;
    slot->state.quiet_state = HOME_AI_ROOM_QUIET_NORMAL;
    slot->state.scene_state = HOME_AI_ROOM_SCENE_NONE;
    slot->state.automation_profile = HOME_AI_ROOM_AUTOMATION_BASIC_ONLY;
    slot->presence_candidate = HOME_AI_ROOM_PRESENCE_UNKNOWN;
    slot->count_candidate_mode = HOME_AI_ROOM_OCCUPANCY_UNKNOWN;
}

static home_ai_room_presence_state_t presence_evidence(const radar_registry_entry_t *entry,
                                                        uint64_t now_ms)
{
    if (entry == NULL || !entry->source_online || !entry->snapshot.frame_fresh ||
        entry->last_report_ms == 0U || now_ms < entry->last_report_ms ||
        now_ms - entry->last_report_ms > RADAR_REGISTRY_FRESHNESS_TIMEOUT_MS) {
        return HOME_AI_ROOM_PRESENCE_UNKNOWN;
    }
    switch (entry->snapshot.state) {
    case RADAR_STATE_MOTION:
    case RADAR_STATE_PRESENT:
    case RADAR_STATE_HOLD:
        return HOME_AI_ROOM_PRESENCE_OCCUPIED;
    case RADAR_STATE_VACANT_INFERRED:
        return HOME_AI_ROOM_PRESENCE_VACANT;
    case RADAR_STATE_UNKNOWN:
    default:
        return HOME_AI_ROOM_PRESENCE_UNKNOWN;
    }
}

static bool radar_is_fresh(const radar_registry_entry_t *entry, uint64_t now_ms)
{
    return entry != NULL && entry->source_online && entry->snapshot.frame_fresh &&
           entry->last_report_ms != 0U && now_ms >= entry->last_report_ms &&
           now_ms - entry->last_report_ms <= RADAR_REGISTRY_FRESHNESS_TIMEOUT_MS;
}

static bool count_is_reliable(radar_person_count_state_t state)
{
    return state == RADAR_PERSON_COUNT_OBSERVED || state == RADAR_PERSON_COUNT_ESTIMATED;
}

static home_ai_room_occupancy_mode_t count_mode(const radar_registry_entry_t *entry,
                                                 home_ai_room_presence_state_t evidence,
                                                 uint8_t *out_count)
{
    if (out_count != NULL) {
        *out_count = 0U;
    }
    if (entry == NULL || evidence != HOME_AI_ROOM_PRESENCE_OCCUPIED ||
        !count_is_reliable(entry->count_summary.count_state)) {
        return HOME_AI_ROOM_OCCUPANCY_UNKNOWN;
    }
    const uint8_t count = entry->count_summary.business_person_count;
    if (out_count != NULL) {
        *out_count = count;
    }
    if (count >= 2U) {
        return HOME_AI_ROOM_OCCUPANCY_MULTIPLE;
    }
    if (count == 1U) {
        return HOME_AI_ROOM_OCCUPANCY_SINGLE;
    }
    return HOME_AI_ROOM_OCCUPANCY_UNKNOWN;
}

static bool update_presence_locked(home_ai_room_state_slot_t *slot,
                                   home_ai_room_presence_state_t evidence,
                                   uint64_t now_ms)
{
    if (evidence == HOME_AI_ROOM_PRESENCE_UNKNOWN) {
        slot->presence_candidate = HOME_AI_ROOM_PRESENCE_UNKNOWN;
        slot->presence_candidate_since_ms = now_ms;
        if (slot->state.presence_state != HOME_AI_ROOM_PRESENCE_UNKNOWN) {
            slot->state.presence_state = HOME_AI_ROOM_PRESENCE_UNKNOWN;
            return true;
        }
        return false;
    }
    if (slot->presence_candidate != evidence) {
        slot->presence_candidate = evidence;
        slot->presence_candidate_since_ms = now_ms;
    }
    const uint32_t confirm_ms = evidence == HOME_AI_ROOM_PRESENCE_OCCUPIED ?
                                    slot->config.occupied_confirm_ms :
                                    slot->config.vacant_confirm_ms;
    if (slot->state.presence_state != evidence &&
        elapsed(now_ms, slot->presence_candidate_since_ms, confirm_ms)) {
        slot->state.presence_state = evidence;
        return true;
    }
    return false;
}

static bool update_count_locked(home_ai_room_state_slot_t *slot,
                                home_ai_room_occupancy_mode_t evidence_mode,
                                uint8_t evidence_count,
                                uint64_t now_ms)
{
    if (evidence_mode == HOME_AI_ROOM_OCCUPANCY_UNKNOWN) {
        slot->count_candidate_mode = HOME_AI_ROOM_OCCUPANCY_UNKNOWN;
        slot->count_candidate = 0U;
        slot->count_candidate_since_ms = now_ms;
        if (slot->state.occupancy_mode != HOME_AI_ROOM_OCCUPANCY_UNKNOWN ||
            slot->state.stable_target_count != 0U) {
            slot->state.occupancy_mode = HOME_AI_ROOM_OCCUPANCY_UNKNOWN;
            slot->state.stable_target_count = 0U;
            return true;
        }
        return false;
    }
    if (slot->count_candidate_mode != evidence_mode ||
        slot->count_candidate != evidence_count) {
        slot->count_candidate_mode = evidence_mode;
        slot->count_candidate = evidence_count;
        slot->count_candidate_since_ms = now_ms;
    }
    const uint32_t confirm_ms = evidence_mode == HOME_AI_ROOM_OCCUPANCY_MULTIPLE ?
                                    slot->config.multiple_confirm_ms :
                                    slot->config.single_confirm_ms;
    if ((slot->state.occupancy_mode != evidence_mode ||
         slot->state.stable_target_count != evidence_count) &&
        elapsed(now_ms, slot->count_candidate_since_ms, confirm_ms)) {
        slot->state.occupancy_mode = evidence_mode;
        slot->state.stable_target_count = evidence_count;
        return true;
    }
    return false;
}

bool home_ai_room_state_init(void)
{
    static const char *const default_room_ids[HOME_AI_ROOM_STATE_COUNT] = {
        "living_room",
        "bedroom_01",
        "bedroom_02",
    };
    static const char *const default_voice_terminal_ids[HOME_AI_ROOM_STATE_COUNT] = {
        "",
        ESP111_PROTOCOL_TERMINAL_DEVICE_ID_C51,
        ESP111_PROTOCOL_TERMINAL_DEVICE_ID_C52,
    };
    ROOM_STATE_LOCK();
    memset(s_slots, 0, sizeof(s_slots));
    for (radar_source_id_t source = RADAR_SOURCE_S3_LOCAL;
         source < RADAR_SOURCE_COUNT;
         source = (radar_source_id_t)(source + 1)) {
        home_ai_room_state_config_t config;
        set_default_config(&config,
                           source,
                           default_room_ids[source],
                           default_voice_terminal_ids[source]);
        reset_slot_locked(&s_slots[source], &config);
    }
    s_initialized = true;
    ROOM_STATE_UNLOCK();
    return true;
}

bool home_ai_room_state_set_config(const home_ai_room_state_config_t *configs, size_t count)
{
    if (configs == NULL || count != HOME_AI_ROOM_STATE_COUNT) {
        return false;
    }
    bool seen[HOME_AI_ROOM_STATE_COUNT] = {false};
    for (size_t index = 0U; index < count; ++index) {
        if (!config_valid(&configs[index]) || seen[configs[index].source]) {
            return false;
        }
        seen[configs[index].source] = true;
    }
    ROOM_STATE_LOCK();
    if (!s_initialized) {
        ROOM_STATE_UNLOCK();
        return false;
    }
    for (size_t index = 0U; index < count; ++index) {
        reset_slot_locked(&s_slots[configs[index].source], &configs[index]);
    }
    ROOM_STATE_UNLOCK();
    return true;
}

bool home_ai_room_state_get_config(radar_source_id_t source, home_ai_room_state_config_t *out)
{
    if (!source_valid(source) || out == NULL) {
        return false;
    }
    ROOM_STATE_LOCK();
    if (!s_initialized) {
        ROOM_STATE_UNLOCK();
        return false;
    }
    *out = s_slots[source].config;
    ROOM_STATE_UNLOCK();
    return true;
}

void home_ai_room_state_update(const radar_registry_entry_t *entries,
                               size_t count,
                               uint64_t now_ms)
{
    if (!s_initialized || now_ms == 0U || (entries == NULL && count > 0U)) {
        return;
    }
    const radar_registry_entry_t *by_source[HOME_AI_ROOM_STATE_COUNT] = {0};
    for (size_t index = 0U; index < count; ++index) {
        if (source_valid(entries[index].source)) {
            by_source[entries[index].source] = &entries[index];
        }
    }
    ROOM_STATE_LOCK();
    for (radar_source_id_t source = RADAR_SOURCE_S3_LOCAL;
         source < RADAR_SOURCE_COUNT;
         source = (radar_source_id_t)(source + 1)) {
        home_ai_room_state_slot_t *slot = &s_slots[source];
        const radar_registry_entry_t *entry = by_source[source];
        const home_ai_room_presence_state_t evidence = presence_evidence(entry, now_ms);
        const bool radar_fresh = radar_is_fresh(entry, now_ms);
        bool changed = slot->state.radar_fresh != radar_fresh;
        slot->state.radar_fresh = radar_fresh;
        changed = update_presence_locked(slot, evidence, now_ms) || changed;
        uint8_t evidence_count = 0U;
        const home_ai_room_occupancy_mode_t evidence_mode =
            count_mode(entry, evidence, &evidence_count);
        changed = update_count_locked(slot, evidence_mode, evidence_count, now_ms) || changed;
        if (changed) {
            slot->state.last_state_change_ms = now_ms;
        }
    }
    ROOM_STATE_UNLOCK();
}

bool home_ai_room_state_set_environment_fresh(radar_source_id_t source, bool fresh, uint64_t now_ms)
{
    if (!source_valid(source) || now_ms == 0U) {
        return false;
    }
    ROOM_STATE_LOCK();
    if (!s_initialized) {
        ROOM_STATE_UNLOCK();
        return false;
    }
    if (s_slots[source].state.environment_fresh != fresh) {
        s_slots[source].state.environment_fresh = fresh;
        s_slots[source].state.last_state_change_ms = now_ms;
    }
    ROOM_STATE_UNLOCK();
    return true;
}

bool home_ai_room_state_set_quiet_state(radar_source_id_t source,
                                        home_ai_room_quiet_state_t quiet_state,
                                        uint64_t now_ms)
{
    if (!source_valid(source) || quiet_state < HOME_AI_ROOM_QUIET_NORMAL ||
        quiet_state > HOME_AI_ROOM_QUIET_TEMPORARY_AWAKE || now_ms == 0U) return false;
    ROOM_STATE_LOCK();
    if (!s_initialized) {
        ROOM_STATE_UNLOCK();
        return false;
    }
    if (s_slots[source].state.quiet_state != quiet_state) {
        s_slots[source].state.quiet_state = quiet_state;
        s_slots[source].state.last_state_change_ms = now_ms;
    }
    ROOM_STATE_UNLOCK();
    return true;
}

bool home_ai_room_state_apply_quiet_schedule(radar_source_id_t source,
                                             bool scheduled,
                                             uint64_t now_ms)
{
    if (!source_valid(source) || now_ms == 0U) return false;
    ROOM_STATE_LOCK();
    if (!s_initialized) {
        ROOM_STATE_UNLOCK();
        return false;
    }
    home_ai_room_quiet_state_t *state = &s_slots[source].state.quiet_state;
    bool changed = false;
    if (scheduled && *state == HOME_AI_ROOM_QUIET_NORMAL) {
        *state = HOME_AI_ROOM_QUIET_SCHEDULED;
        changed = true;
    } else if (!scheduled && *state == HOME_AI_ROOM_QUIET_SCHEDULED) {
        *state = HOME_AI_ROOM_QUIET_NORMAL;
        changed = true;
    }
    if (changed) s_slots[source].state.last_state_change_ms = now_ms;
    ROOM_STATE_UNLOCK();
    return true;
}

bool home_ai_room_state_get(radar_source_id_t source, home_ai_room_state_t *out)
{
    if (!source_valid(source) || out == NULL) {
        return false;
    }
    ROOM_STATE_LOCK();
    if (!s_initialized) {
        ROOM_STATE_UNLOCK();
        return false;
    }
    *out = s_slots[source].state;
    ROOM_STATE_UNLOCK();
    return true;
}

size_t home_ai_room_state_snapshot(home_ai_room_state_t *out, size_t capacity)
{
    if (out == NULL || capacity == 0U) {
        return 0U;
    }
    ROOM_STATE_LOCK();
    if (!s_initialized) {
        ROOM_STATE_UNLOCK();
        return 0U;
    }
    const size_t result = capacity < HOME_AI_ROOM_STATE_COUNT ?
                              capacity : HOME_AI_ROOM_STATE_COUNT;
    for (size_t index = 0U; index < result; ++index) {
        out[index] = s_slots[index].state;
    }
    ROOM_STATE_UNLOCK();
    return result;
}

const char *home_ai_room_presence_state_name(home_ai_room_presence_state_t state)
{
    switch (state) {
    case HOME_AI_ROOM_PRESENCE_OCCUPIED:
        return "occupied";
    case HOME_AI_ROOM_PRESENCE_VACANT:
        return "vacant";
    case HOME_AI_ROOM_PRESENCE_UNKNOWN:
    default:
        return "unknown";
    }
}

const char *home_ai_room_occupancy_mode_name(home_ai_room_occupancy_mode_t mode)
{
    switch (mode) {
    case HOME_AI_ROOM_OCCUPANCY_SINGLE:
        return "single";
    case HOME_AI_ROOM_OCCUPANCY_MULTIPLE:
        return "multiple";
    case HOME_AI_ROOM_OCCUPANCY_UNKNOWN:
    default:
        return "unknown";
    }
}
