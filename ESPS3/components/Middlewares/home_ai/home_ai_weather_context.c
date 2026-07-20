#include "home_ai_weather_context.h"

#include <stddef.h>

#include "esp111_protocol_common.h"

static bool parse_uint64(cJSON *item, uint64_t *out_value)
{
    if (!cJSON_IsNumber(item) || out_value == NULL || item->valuedouble < 0.0 ||
        item->valuedouble > 9007199254740991.0) {
        return false;
    }
    const uint64_t value = (uint64_t)item->valuedouble;
    if (item->valuedouble != (double)value) return false;
    *out_value = value;
    return true;
}

static bool parse_optional_uint64(cJSON *object,
                                  const char *name,
                                  bool *out_present,
                                  uint64_t *out_value)
{
    if (out_present == NULL || out_value == NULL) return false;
    *out_present = false;
    *out_value = 0U;
    cJSON *item = cJSON_IsObject(object) ? cJSON_GetObjectItemCaseSensitive(object, name) : NULL;
    if (item == NULL || cJSON_IsNull(item)) return true;
    if (!parse_uint64(item, out_value)) return false;
    *out_present = true;
    return true;
}

bool home_ai_weather_context_parse(cJSON *payload_root,
                                   uint64_t server_time_ms,
                                   uint64_t local_time_ms,
                                   bool *out_available,
                                   bool *out_dark,
                                   uint64_t *out_expires_at_uptime_ms)
{
    if (out_available == NULL || out_dark == NULL || out_expires_at_uptime_ms == NULL) {
        return false;
    }
    *out_available = false;
    *out_dark = false;
    *out_expires_at_uptime_ms = 0U;

    cJSON *weather = cJSON_IsObject(payload_root) ?
        cJSON_GetObjectItemCaseSensitive(payload_root,
                                         ESP111_PROTOCOL_HOME_AI_JSON_WEATHER_CONTEXT) : NULL;
    if (weather == NULL) return true;
    if (!cJSON_IsObject(weather)) return false;

    bool observed_present = false;
    bool expires_present = false;
    uint64_t observed_at_ms = 0U;
    uint64_t expires_at_ms = 0U;
    if (!parse_optional_uint64(weather,
                               ESP111_PROTOCOL_HOME_AI_JSON_WEATHER_OBSERVED_AT_MS,
                               &observed_present,
                               &observed_at_ms) ||
        !parse_optional_uint64(weather,
                               ESP111_PROTOCOL_HOME_AI_JSON_WEATHER_EXPIRES_AT_MS,
                               &expires_present,
                               &expires_at_ms)) {
        return false;
    }
    if (observed_present && expires_present && expires_at_ms < observed_at_ms) return false;

    cJSON *available = cJSON_GetObjectItemCaseSensitive(
        weather,
        ESP111_PROTOCOL_HOME_AI_JSON_WEATHER_AVAILABLE);
    cJSON *dark = cJSON_GetObjectItemCaseSensitive(
        weather,
        ESP111_PROTOCOL_HOME_AI_JSON_WEATHER_DARK);
    if (available == NULL || cJSON_IsNull(available)) return true;
    if (!cJSON_IsBool(available)) return false;
    if (dark != NULL && !cJSON_IsNull(dark) && !cJSON_IsBool(dark)) return false;
    if (!cJSON_IsTrue(available)) return true;
    if (dark == NULL || cJSON_IsNull(dark) || !observed_present || !expires_present ||
        observed_at_ms > server_time_ms || expires_at_ms <= server_time_ms ||
        expires_at_ms <= observed_at_ms) {
        return false;
    }
    const uint64_t ttl_ms = expires_at_ms - server_time_ms;
    if (ttl_ms > UINT64_MAX - local_time_ms) return false;
    *out_available = true;
    *out_dark = cJSON_IsTrue(dark);
    *out_expires_at_uptime_ms = local_time_ms + ttl_ms;
    return true;
}
