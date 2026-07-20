#ifndef HOME_AI_VOICE_ROUTER_H
#define HOME_AI_VOICE_ROUTER_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HOME_AI_VOICE_ROUTER_MAX_INFLIGHT 8U
#define HOME_AI_VOICE_ROUTER_PROMPT_LEN 96U

typedef enum {
    HOME_AI_VOICE_ROUTE_QUEUED = 0,
    HOME_AI_VOICE_ROUTE_SUPPRESSED_NO_PRESENCE,
    HOME_AI_VOICE_ROUTE_SUPPRESSED_QUIET,
    HOME_AI_VOICE_ROUTE_SUPPRESSED_BUSY,
    HOME_AI_VOICE_ROUTE_SUPPRESSED_RATE_LIMIT,
    HOME_AI_VOICE_ROUTE_SUPPRESSED_NO_TERMINAL,
    HOME_AI_VOICE_ROUTE_REJECTED_RESOURCE,
    HOME_AI_VOICE_ROUTE_REJECTED_INVALID,
} home_ai_voice_route_status_t;

typedef struct {
    home_ai_voice_route_status_t status;
    uint32_t playback_generation;
    uint8_t queued_count;
    bool emergency;
    char emergency_event_id[64];
    char reason[64];
} home_ai_voice_route_result_t;

typedef struct {
    bool matched;
    bool completed;
    bool emergency;
    char emergency_event_id[64];
} home_ai_voice_ack_result_t;

bool home_ai_voice_router_init(void);
void home_ai_voice_router_tick(uint64_t now_ms);

/* Release one pending terminal ACK for the matching playback generation. */
bool home_ai_voice_router_acknowledge(uint32_t playback_generation);
bool home_ai_voice_router_acknowledge_ex(uint32_t playback_generation,
                                         home_ai_voice_ack_result_t *out_result);

/* Stop the active local voice session and queue a bounded C5 playback abort. */
esp_err_t home_ai_voice_router_request_stop(const char *room_id);

/* Queue a bounded prompt through the existing S3 -> C5 command router. */
esp_err_t home_ai_voice_router_request_prompt(const char *room_id,
                                              const char *prompt,
                                              const char *decision_id,
                                              bool emergency,
                                              uint64_t now_ms,
                                              home_ai_voice_route_result_t *out_result);
esp_err_t home_ai_voice_router_request_prompt_with_event_id(
    const char *room_id,
    const char *prompt,
    const char *decision_id,
    bool emergency,
    const char *emergency_event_id,
    uint64_t now_ms,
    home_ai_voice_route_result_t *out_result);

/* Local command hooks used by the constrained offline voice vocabulary. */
bool home_ai_voice_router_set_mute(const char *room_id, bool muted, uint64_t until_ms);
bool home_ai_voice_router_set_temporary_awake(const char *room_id,
                                              uint64_t duration_ms,
                                              uint64_t now_ms);
bool home_ai_voice_router_clear_temporary_awake(const char *room_id, uint64_t now_ms);

const char *home_ai_voice_route_status_name(home_ai_voice_route_status_t status);

#ifdef __cplusplus
}
#endif

#endif /* HOME_AI_VOICE_ROUTER_H */
