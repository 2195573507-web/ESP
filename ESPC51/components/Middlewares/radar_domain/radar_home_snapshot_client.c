#include "radar_home_snapshot_client.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "esp111_protocol_common.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "server_comm_http.h"

#define RADAR_HOME_SNAPSHOT_POLL_MS 1000U
#define RADAR_HOME_SNAPSHOT_HTTP_TIMEOUT_MS 750U
#define RADAR_HOME_SNAPSHOT_RESPONSE_BYTES 1024U

static const char *TAG = "radar_home_ui";
static char s_response[RADAR_HOME_SNAPSHOT_RESPONSE_BYTES];
static radar_home_snapshot_t s_snapshot;
static radar_home_snapshot_client_stats_t s_stats;
static uint64_t s_next_poll_ms;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static const char *skip_space(const char *cursor)
{
    while (cursor != NULL && isspace((unsigned char)*cursor)) {
        ++cursor;
    }
    return cursor;
}

static const char *find_key(const char *json, const char *key)
{
    if (json == NULL || key == NULL) return NULL;
    const size_t key_len = strlen(key);
    for (const char *cursor = json; (cursor = strchr(cursor, '"')) != NULL; ++cursor) {
        if (strncmp(cursor + 1U, key, key_len) != 0 || cursor[key_len + 1U] != '"') continue;
        const char *value = skip_space(cursor + key_len + 2U);
        if (value != NULL && *value == ':') return skip_space(value + 1U);
    }
    return NULL;
}

static const char *find_matching(const char *start, char open, char close)
{
    if (start == NULL || *start != open) return NULL;
    unsigned depth = 0U;
    bool quoted = false;
    bool escaped = false;
    for (const char *cursor = start; *cursor != '\0'; ++cursor) {
        const char value = *cursor;
        if (quoted) {
            if (escaped) escaped = false;
            else if (value == '\\') escaped = true;
            else if (value == '"') quoted = false;
            continue;
        }
        if (value == '"') quoted = true;
        else if (value == open) ++depth;
        else if (value == close && --depth == 0U) return cursor;
    }
    return NULL;
}

static bool get_bool(const char *json, const char *key, bool *out)
{
    const char *value = find_key(json, key);
    if (value == NULL || out == NULL) return false;
    /* S3's compact snapshot envelope uses the established numeric ok=0/1
     * form, while state fields use JSON booleans. Accept both representations
     * without relaxing any of the required source or home fields. */
    if (*value == '1') {
        *out = true;
        return true;
    }
    if (*value == '0') {
        *out = false;
        return true;
    }
    if (strncmp(value, "true", 4U) == 0) {
        *out = true;
        return true;
    }
    if (strncmp(value, "false", 5U) == 0) {
        *out = false;
        return true;
    }
    return false;
}

static bool get_u8(const char *json, const char *key, uint8_t *out)
{
    const char *value = find_key(json, key);
    if (value == NULL || out == NULL) return false;
    char *end = NULL;
    const unsigned long parsed = strtoul(value, &end, 10);
    if (end == value || parsed > UINT8_MAX) return false;
    *out = (uint8_t)parsed;
    return true;
}

static bool get_string(const char *json, const char *key, char *out, size_t out_size)
{
    const char *value = find_key(json, key);
    if (value == NULL || out == NULL || out_size == 0U || *value != '"') return false;
    size_t length = 0U;
    bool escaped = false;
    for (++value; *value != '\0'; ++value) {
        char ch = *value;
        if (escaped) {
            escaped = false;
        } else if (ch == '\\') {
            escaped = true;
            continue;
        } else if (ch == '"') {
            out[length] = '\0';
            return true;
        }
        if (length + 1U < out_size) out[length++] = ch;
    }
    out[length] = '\0';
    return false;
}

static radar_home_motion_t parse_motion(const char *motion)
{
    if (strcmp(motion, "moving") == 0) return RADAR_HOME_MOTION_MOVING;
    if (strcmp(motion, "still_candidate") == 0) return RADAR_HOME_MOTION_STILL_CANDIDATE;
    if (strcmp(motion, "none") == 0) return RADAR_HOME_MOTION_NONE;
    return RADAR_HOME_MOTION_UNKNOWN;
}

static bool parse_source(const char *object, radar_home_snapshot_source_t *out)
{
    char motion[20] = {0};
    return get_u8(object, "source_id", &out->source_id) &&
           get_string(object, "source", out->source, sizeof(out->source)) &&
           get_string(object, "room", out->room, sizeof(out->room)) &&
           get_bool(object, "online", &out->online) &&
           get_bool(object, "occupied", &out->occupied) &&
           get_string(object, "motion", motion, sizeof(motion)) &&
           get_u8(object, "person_count", &out->person_count) &&
           ((out->motion = parse_motion(motion)), true);
}

static bool parse_snapshot(const char *json, uint64_t received_at_ms, radar_home_snapshot_t *out)
{
    bool ok = false;
    const char *sources = NULL;
    const char *home = NULL;
    if (json == NULL || out == NULL || !get_bool(json, "ok", &ok) || !ok ||
        (sources = find_key(json, "sources")) == NULL || *sources != '[' ||
        (home = find_key(json, "home")) == NULL || *home != '{') return false;

    radar_home_snapshot_t parsed = {.valid = true, .received_at_ms = received_at_ms};
    const char *array_end = find_matching(sources, '[', ']');
    if (array_end == NULL) return false;
    const char *cursor = sources + 1U;
    while (cursor != NULL && cursor < array_end) {
        cursor = strchr(cursor, '{');
        if (cursor == NULL || cursor >= array_end) break;
        const char *object_end = find_matching(cursor, '{', '}');
        if (object_end == NULL || object_end > array_end ||
            parsed.source_count >= RADAR_HOME_SNAPSHOT_MAX_SOURCES ||
            !parse_source(cursor, &parsed.sources[parsed.source_count])) return false;
        ++parsed.source_count;
        cursor = object_end + 1U;
    }
    if (!get_bool(home, "known", &parsed.home_known) ||
        !get_bool(home, "occupied", &parsed.home_occupied) ||
        !get_u8(home, "person_count", &parsed.home_person_count) ||
        !get_u8(home, "room_count", &parsed.home_room_count)) return false;
    *out = parsed;
    return true;
}

void radar_home_snapshot_client_poll(uint64_t now_ms)
{
    if (s_next_poll_ms != 0U && now_ms < s_next_poll_ms) return;
    s_next_poll_ms = now_ms + RADAR_HOME_SNAPSHOT_POLL_MS;
    ++s_stats.request_count;

    server_comm_http_response_t response = {0};
    const esp_err_t ret = server_comm_http_get_json(ESP111_PROTOCOL_ROUTE_RADAR_HOME_SNAPSHOT,
                                                     RADAR_HOME_SNAPSHOT_HTTP_TIMEOUT_MS,
                                                     s_response,
                                                     sizeof(s_response),
                                                     &response);
    if (ret != ESP_OK) {
        ++s_stats.transport_error_count;
        ESP_LOGW(TAG, "snapshot poll deferred ret=%s status=%d", esp_err_to_name(ret),
                 response.status_code);
        return;
    }

    radar_home_snapshot_t parsed = {0};
    if (!parse_snapshot(s_response, now_ms, &parsed)) {
        ++s_stats.parse_error_count;
        ESP_LOGW(TAG, "snapshot rejected: compact UI contract mismatch");
        return;
    }
    portENTER_CRITICAL(&s_lock);
    parsed.generation = s_snapshot.generation + 1U;
    if (parsed.generation == 0U) parsed.generation = 1U;
    s_snapshot = parsed;
    portEXIT_CRITICAL(&s_lock);
    ++s_stats.success_count;
}

bool radar_home_snapshot_client_get(radar_home_snapshot_t *out)
{
    if (out == NULL) return false;
    portENTER_CRITICAL(&s_lock);
    *out = s_snapshot;
    portEXIT_CRITICAL(&s_lock);
    return out->valid;
}

void radar_home_snapshot_client_get_stats(radar_home_snapshot_client_stats_t *out)
{
    if (out == NULL) return;
    portENTER_CRITICAL(&s_lock);
    *out = s_stats;
    portEXIT_CRITICAL(&s_lock);
}
