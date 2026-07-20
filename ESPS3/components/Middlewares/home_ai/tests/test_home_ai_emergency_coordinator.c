#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "environment_alarm_reporter.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "home_ai_emergency_coordinator.h"
#include "home_ai_event_reporter.h"
#include "home_ai_voice_router.h"

static home_ai_voice_route_result_t s_route;
static unsigned int s_route_calls;
static char s_reported_state[32];
static char s_reported_event[HOME_AI_EMERGENCY_EVENT_ID_LEN];
static int64_t s_now_us;

int64_t esp_timer_get_time(void)
{
    return s_now_us;
}

SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *storage)
{
    return storage;
}

int xSemaphoreTake(SemaphoreHandle_t semaphore, unsigned int wait_ticks)
{
    (void)semaphore;
    (void)wait_ticks;
    return pdTRUE;
}

int xSemaphoreGive(SemaphoreHandle_t semaphore)
{
    (void)semaphore;
    return pdTRUE;
}

esp_err_t environment_alarm_reporter_set_observer(environment_alarm_observer_t observer,
                                                   void *context)
{
    (void)observer;
    (void)context;
    return ESP_OK;
}

void home_ai_event_reporter_record_emergency(const char *event_id,
                                             const char *room_id,
                                             const char *state,
                                             uint8_t priority,
                                             uint64_t occurred_at_ms)
{
    (void)room_id;
    (void)priority;
    (void)occurred_at_ms;
    snprintf(s_reported_event, sizeof(s_reported_event), "%s", event_id != NULL ? event_id : "");
    snprintf(s_reported_state, sizeof(s_reported_state), "%s", state != NULL ? state : "");
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
    (void)room_id;
    (void)prompt;
    (void)decision_id;
    (void)emergency;
    (void)emergency_event_id;
    (void)now_ms;
    ++s_route_calls;
    *out_result = s_route;
    return ESP_OK;
}

static alarm_event_t event(uint64_t alarm_id,
                           alarm_event_status_t status,
                           alarm_level_t level,
                           alarm_reason_t reason,
                           uint64_t at)
{
    alarm_event_t value = {0};
    value.alarm_id = alarm_id;
    value.status = status;
    value.alarm_level = level;
    value.reason = reason;
    value.event_monotonic_ms = at;
    snprintf(value.room_id, sizeof(value.room_id), "bedroom_01");
    return value;
}

int main(void)
{
    assert(home_ai_emergency_coordinator_init());
    s_route.status = HOME_AI_VOICE_ROUTE_QUEUED;
    s_route.queued_count = 1U;
    s_route.playback_generation = 11U;

    alarm_event_t active = event(0x10U,
                                 ALARM_STATUS_ACTIVE,
                                 ALARM_LEVEL_CRITICAL,
                                 ALARM_REASON_THRESHOLD_CONFIRMED,
                                 1000U);
    home_ai_emergency_coordinator_on_alarm_event(&active, NULL);
    home_ai_emergency_snapshot_t snapshot = {0};
    assert(home_ai_emergency_coordinator_snapshot(&snapshot, 1U) == 1U);
    assert(snapshot.state == HOME_AI_EMERGENCY_DETECTED);
    assert(strcmp(snapshot.event_id, "env_0000000000000010") == 0);

    home_ai_emergency_coordinator_tick(1000U);
    assert(s_route_calls == 1U);
    assert(strcmp(s_reported_state, "ACTIVE_UNACKNOWLEDGED") == 0);
    assert(home_ai_emergency_coordinator_playback_completed(snapshot.event_id,
                                                            11U,
                                                            true,
                                                            2000U));
    assert(home_ai_emergency_coordinator_acknowledge_user(snapshot.event_id, 3000U));
    assert(home_ai_emergency_coordinator_snapshot(&snapshot, 1U) == 1U);
    assert(snapshot.state == HOME_AI_EMERGENCY_ACKNOWLEDGED);

    active.reason = ALARM_REASON_LEVEL_ESCALATED;
    home_ai_emergency_coordinator_on_alarm_event(&active, NULL);
    assert(home_ai_emergency_coordinator_snapshot(&snapshot, 1U) == 1U);
    assert(snapshot.state == HOME_AI_EMERGENCY_ESCALATED);
    s_route.playback_generation = 12U;
    home_ai_emergency_coordinator_tick(3000U);
    assert(s_route_calls == 2U);

    alarm_event_t recovered = event(0x10U,
                                    ALARM_STATUS_RECOVERED,
                                    ALARM_LEVEL_CRITICAL,
                                    ALARM_REASON_RECOVERY_CONFIRMED,
                                    4000U);
    home_ai_emergency_coordinator_on_alarm_event(&recovered, NULL);
    assert(home_ai_emergency_coordinator_snapshot(&snapshot, 1U) == 1U);
    assert(snapshot.state == HOME_AI_EMERGENCY_RECOVERING);
    s_route.playback_generation = 13U;
    home_ai_emergency_coordinator_tick(4000U);
    assert(s_route_calls == 3U);
    assert(home_ai_emergency_coordinator_playback_completed(snapshot.event_id,
                                                            13U,
                                                            true,
                                                            5000U));
    assert(home_ai_emergency_coordinator_snapshot(&snapshot, 1U) == 1U);
    assert(snapshot.state == HOME_AI_EMERGENCY_RESOLVED);

    alarm_event_t warning = event(0x20U,
                                  ALARM_STATUS_ACTIVE,
                                  ALARM_LEVEL_WARNING,
                                  ALARM_REASON_THRESHOLD_CONFIRMED,
                                  6000U);
    home_ai_emergency_coordinator_on_alarm_event(&warning, NULL);
    assert(home_ai_emergency_coordinator_snapshot(&snapshot, 8U) == 1U);

    assert(home_ai_emergency_coordinator_init());
    const home_ai_emergency_acknowledgement_t acknowledgement = {
        .event_id = "env_0000000000000030",
        .acknowledged_at_ms = 7000U,
    };
    assert(home_ai_emergency_coordinator_replace_acknowledgements(&acknowledgement, 1U, 7000U));
    alarm_event_t preacknowledged = event(0x30U,
                                         ALARM_STATUS_ACTIVE,
                                         ALARM_LEVEL_CRITICAL,
                                         ALARM_REASON_THRESHOLD_CONFIRMED,
                                         8000U);
    home_ai_emergency_coordinator_on_alarm_event(&preacknowledged, NULL);
    assert(home_ai_emergency_coordinator_snapshot(&snapshot, 1U) == 1U);
    assert(snapshot.state == HOME_AI_EMERGENCY_ACKNOWLEDGED);

    puts("home ai emergency coordinator host tests: PASS");
    return 0;
}
