#ifndef HOME_AI_WEATHER_CONTEXT_H
#define HOME_AI_WEATHER_CONTEXT_H

#include <stdbool.h>
#include <stdint.h>

#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

bool home_ai_weather_context_parse(cJSON *payload_root,
                                   uint64_t server_time_ms,
                                   uint64_t local_time_ms,
                                   bool *out_available,
                                   bool *out_dark,
                                   uint64_t *out_expires_at_uptime_ms);

#ifdef __cplusplus
}
#endif

#endif /* HOME_AI_WEATHER_CONTEXT_H */
