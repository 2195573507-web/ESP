#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "home_ai_voice_router.h"
#include "home_ai_event_reporter.h"
#include "home_ai_room_state.h"
#include "home_ai_voice_session.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define TEST_MAX_QUEUED 64U

static home_ai_room_state_t s_states[HOME_AI_ROOM_STATE_COUNT];
static home_ai_room_state_config_t s_configs[HOME_AI_ROOM_STATE_COUNT];
static bool s_session_busy;
static bool s_fail_all_enqueues;
static unsigned int s_fail_enqueue_attempt;
static unsigned int s_enqueue_attempts;
static unsigned int s_queue_count;
static char s_targets[TEST_MAX_QUEUED][HOME_AI_ROOM_STATE_VOICE_TERMINAL_ID_LEN];
static char s_command_types[TEST_MAX_QUEUED][32];
static char s_params[TEST_MAX_QUEUED][512];
static char s_sources[TEST_MAX_QUEUED][32];
static unsigned int s_mutex_depth;

SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *storage)
{
    assert(storage != NULL);
    return storage;
}

int xSemaphoreTake(SemaphoreHandle_t semaphore, unsigned int wait_ticks)
{
    (void)wait_ticks;
    assert(semaphore != NULL);
    assert(s_mutex_depth == 0U);
    s_mutex_depth = 1U;
    return pdTRUE;
}

int xSemaphoreGive(SemaphoreHandle_t semaphore)
{
    assert(semaphore != NULL);
    assert(s_mutex_depth == 1U);
    s_mutex_depth = 0U;
    return pdTRUE;
}

uint32_t esp_random(void)
{
    return 0x12345678U;
}

esp_err_t command_router_enqueue(const char *target_device_id,
                                 const char *command_type,
                                 const char *params_json,
                                 const char *source)
{
    assert(strcmp(command_type, "speaker.play_audio") == 0 ||
           strcmp(command_type, "speaker.stop_audio") == 0);
    assert(source != NULL);
    ++s_enqueue_attempts;
    if (s_fail_all_enqueues || s_enqueue_attempts == s_fail_enqueue_attempt) {
        return ESP_ERR_NO_MEM;
    }
    assert(s_queue_count < TEST_MAX_QUEUED);
    snprintf(s_targets[s_queue_count], sizeof(s_targets[s_queue_count]), "%s", target_device_id);
    snprintf(s_command_types[s_queue_count], sizeof(s_command_types[s_queue_count]), "%s", command_type);
    snprintf(s_params[s_queue_count], sizeof(s_params[s_queue_count]), "%s", params_json);
    snprintf(s_sources[s_queue_count], sizeof(s_sources[s_queue_count]), "%s", source);
    ++s_queue_count;
    return ESP_OK;
}

bool home_ai_room_state_get(radar_source_id_t source, home_ai_room_state_t *out)
{
    if (source < RADAR_SOURCE_S3_LOCAL || source >= RADAR_SOURCE_COUNT || out == NULL) return false;
    *out = s_states[source];
    return true;
}

bool home_ai_room_state_get_config(radar_source_id_t source,
                                   home_ai_room_state_config_t *out)
{
    if (source < RADAR_SOURCE_S3_LOCAL || source >= RADAR_SOURCE_COUNT || out == NULL) return false;
    *out = s_configs[source];
    return true;
}

bool home_ai_room_state_set_quiet_state(radar_source_id_t source,
                                        home_ai_room_quiet_state_t quiet_state,
                                        uint64_t now_ms)
{
    if (source >= RADAR_SOURCE_COUNT || now_ms == 0U) return false;
    s_states[source].quiet_state = quiet_state;
    return true;
}

bool home_ai_voice_session_get(home_ai_voice_session_lease_t *out_lease)
{
    if (out_lease != NULL) memset(out_lease, 0, sizeof(*out_lease));
    return s_session_busy;
}

bool home_ai_voice_session_preempt_for_emergency(home_ai_voice_session_lease_t *out_preempted)
{
    if (out_preempted != NULL) memset(out_preempted, 0, sizeof(*out_preempted));
    const bool was_busy = s_session_busy;
    s_session_busy = false;
    return was_busy;
}

void home_ai_event_reporter_record_emergency(const char *event_id,
                                             const char *room_id,
                                             const char *state,
                                             uint8_t priority,
                                             uint64_t occurred_at_ms)
{
    assert(event_id != NULL && room_id != NULL && state != NULL);
    assert(priority > 0U && occurred_at_ms > 0U);
}

static void reset_fixture(void)
{
    memset(s_states, 0, sizeof(s_states));
    memset(s_configs, 0, sizeof(s_configs));
    strcpy(s_states[RADAR_SOURCE_S3_LOCAL].room_id, "living_room");
    strcpy(s_states[RADAR_SOURCE_C51].room_id, "bedroom_01");
    strcpy(s_states[RADAR_SOURCE_C52].room_id, "bedroom_02");
    for (radar_source_id_t source = RADAR_SOURCE_S3_LOCAL;
         source < RADAR_SOURCE_COUNT;
         source = (radar_source_id_t)(source + 1)) {
        s_configs[source].source = source;
        strcpy(s_configs[source].room_id, s_states[source].room_id);
    }
    strcpy(s_configs[RADAR_SOURCE_C51].voice_terminal_device_id, "sensair_shuttle_01");
    strcpy(s_configs[RADAR_SOURCE_C52].voice_terminal_device_id, "sensair_shuttle_02");
    s_states[RADAR_SOURCE_S3_LOCAL].presence_state = HOME_AI_ROOM_PRESENCE_OCCUPIED;
    s_states[RADAR_SOURCE_C51].presence_state = HOME_AI_ROOM_PRESENCE_OCCUPIED;
    s_states[RADAR_SOURCE_C52].presence_state = HOME_AI_ROOM_PRESENCE_VACANT;
    s_session_busy = false;
    s_fail_all_enqueues = false;
    s_fail_enqueue_attempt = 0U;
    s_enqueue_attempts = 0U;
    s_queue_count = 0U;
    memset(s_targets, 0, sizeof(s_targets));
    memset(s_command_types, 0, sizeof(s_command_types));
    memset(s_params, 0, sizeof(s_params));
    memset(s_sources, 0, sizeof(s_sources));
    assert(home_ai_voice_router_init());
}

static void test_stop_routes_to_bound_terminal(void)
{
    reset_fixture();
    s_session_busy = true;
    assert(home_ai_voice_router_request_stop("bedroom_01") == ESP_OK);
    assert(!s_session_busy);
    assert(s_queue_count == 1U);
    assert(strcmp(s_targets[0], "sensair_shuttle_01") == 0);
    assert(strcmp(s_command_types[0], "speaker.stop_audio") == 0);
    assert(strcmp(s_params[0], "{\"reason\":\"offline_voice_stop\"}") == 0);
    assert(strcmp(s_sources[0], "home_ai_offline_voice") == 0);

    reset_fixture();
    strcpy(s_configs[RADAR_SOURCE_C51].voice_terminal_device_id, "sensair_shuttle_02");
    assert(home_ai_voice_router_request_stop("bedroom_01") == ESP_OK);
    assert(strcmp(s_targets[0], "sensair_shuttle_02") == 0);

    reset_fixture();
    s_session_busy = true;
    s_configs[RADAR_SOURCE_C51].voice_terminal_device_id[0] = '\0';
    assert(home_ai_voice_router_request_stop("bedroom_01") == ESP_ERR_NOT_FOUND);
    assert(s_session_busy);
    assert(s_queue_count == 0U);
    assert(home_ai_voice_router_request_stop("unknown_room") == ESP_ERR_NOT_FOUND);

    reset_fixture();
    s_session_busy = true;
    s_fail_all_enqueues = true;
    assert(home_ai_voice_router_request_stop("bedroom_01") == ESP_ERR_NO_MEM);
    assert(!s_session_busy);
    assert(s_queue_count == 0U);
}

static void test_normal_routing_and_policy(void)
{
    reset_fixture();
    home_ai_voice_route_result_t route = {0};
    assert(home_ai_voice_router_request_prompt("bedroom_01",
                                               "light on",
                                               "decision_1",
                                               false,
                                               1000U,
                                               &route) == ESP_OK);
    assert(route.status == HOME_AI_VOICE_ROUTE_QUEUED);
    assert(route.queued_count == 1U);
    assert(strcmp(s_targets[0], "sensair_shuttle_01") == 0);
    assert(strstr(s_params[0], "\"playback_generation\":1") != NULL);

    assert(home_ai_voice_router_request_prompt("bedroom_01",
                                               "light on",
                                               "decision_2",
                                               false,
                                               2000U,
                                               &route) == ESP_OK);
    assert(route.status == HOME_AI_VOICE_ROUTE_SUPPRESSED_RATE_LIMIT);

    s_session_busy = true;
    s_states[RADAR_SOURCE_C52].presence_state = HOME_AI_ROOM_PRESENCE_UNKNOWN;
    assert(home_ai_voice_router_request_prompt("living_room",
                                               "danger",
                                               "decision_3",
                                               true,
                                               40000U,
                                               &route) == ESP_OK);
    assert(route.status == HOME_AI_VOICE_ROUTE_QUEUED);
    assert(route.queued_count == 2U);
    assert(!s_session_busy);
    assert(route.emergency_event_id[0] != '\0');

    s_states[RADAR_SOURCE_C51].quiet_state = HOME_AI_ROOM_QUIET_SCHEDULED;
    assert(home_ai_voice_router_set_temporary_awake("bedroom_01", 5000U, 50000U));
    assert(s_states[RADAR_SOURCE_C51].quiet_state == HOME_AI_ROOM_QUIET_TEMPORARY_AWAKE);
    home_ai_voice_router_tick(55000U);
    assert(s_states[RADAR_SOURCE_C51].quiet_state == HOME_AI_ROOM_QUIET_NORMAL);

    reset_fixture();
    strcpy(s_states[RADAR_SOURCE_C51].room_id, "sleep_room");
    strcpy(s_configs[RADAR_SOURCE_C51].room_id, "sleep_room");
    assert(home_ai_voice_router_request_prompt("sleep_room",
                                               "dynamic room",
                                               "decision_4",
                                               false,
                                               60000U,
                                               &route) == ESP_OK);
    assert(route.status == HOME_AI_VOICE_ROUTE_QUEUED);
    assert(strcmp(s_targets[0], "sensair_shuttle_01") == 0);
    assert(strstr(s_params[0], "\"room_id\":\"sleep_room\"") != NULL);
}

static void test_dynamic_terminal_binding_and_empty_terminal(void)
{
    reset_fixture();
    strcpy(s_configs[RADAR_SOURCE_C51].voice_terminal_device_id, "sensair_shuttle_02");
    s_configs[RADAR_SOURCE_C52].voice_terminal_device_id[0] = '\0';
    home_ai_voice_route_result_t route = {0};
    assert(home_ai_voice_router_request_prompt("bedroom_01",
                                               "rebound terminal",
                                               "decision_rebind",
                                               false,
                                               1000U,
                                               &route) == ESP_OK);
    assert(route.status == HOME_AI_VOICE_ROUTE_QUEUED);
    assert(strcmp(s_targets[0], "sensair_shuttle_02") == 0);

    reset_fixture();
    s_configs[RADAR_SOURCE_C51].voice_terminal_device_id[0] = '\0';
    for (unsigned int index = 0U; index < HOME_AI_VOICE_ROUTER_MAX_INFLIGHT + 2U; ++index) {
        assert(home_ai_voice_router_request_prompt("bedroom_01",
                                                   "no terminal",
                                                   "decision_empty",
                                                   false,
                                                   1000U,
                                                   &route) == ESP_OK);
        assert(route.status == HOME_AI_VOICE_ROUTE_SUPPRESSED_NO_TERMINAL);
        assert(strcmp(route.reason, "suppressed_no_terminal") == 0);
        assert(route.playback_generation == 0U);
    }
    assert(s_enqueue_attempts == 0U);
    strcpy(s_configs[RADAR_SOURCE_C51].voice_terminal_device_id, "sensair_shuttle_01");
    assert(home_ai_voice_router_request_prompt("bedroom_01",
                                               "terminal restored",
                                               "decision_restored",
                                               false,
                                               1000U,
                                               &route) == ESP_OK);
    assert(route.status == HOME_AI_VOICE_ROUTE_QUEUED);
    assert(route.playback_generation == 1U);
}

static void test_emergency_matrix(void)
{
    typedef struct {
        home_ai_room_presence_state_t c51;
        home_ai_room_presence_state_t c52;
        unsigned int expected_count;
        const char *first_target;
        const char *last_target;
    } emergency_case_t;
    const emergency_case_t cases[] = {
        {HOME_AI_ROOM_PRESENCE_OCCUPIED, HOME_AI_ROOM_PRESENCE_VACANT, 1U,
         "sensair_shuttle_01", "sensair_shuttle_01"},
        {HOME_AI_ROOM_PRESENCE_VACANT, HOME_AI_ROOM_PRESENCE_OCCUPIED, 1U,
         "sensair_shuttle_02", "sensair_shuttle_02"},
        {HOME_AI_ROOM_PRESENCE_OCCUPIED, HOME_AI_ROOM_PRESENCE_OCCUPIED, 2U,
         "sensair_shuttle_01", "sensair_shuttle_02"},
        {HOME_AI_ROOM_PRESENCE_VACANT, HOME_AI_ROOM_PRESENCE_VACANT, 0U, NULL, NULL},
        {HOME_AI_ROOM_PRESENCE_UNKNOWN, HOME_AI_ROOM_PRESENCE_UNKNOWN, 2U,
         "sensair_shuttle_01", "sensair_shuttle_02"},
        {HOME_AI_ROOM_PRESENCE_UNKNOWN, HOME_AI_ROOM_PRESENCE_OCCUPIED, 2U,
         "sensair_shuttle_01", "sensair_shuttle_02"},
        {HOME_AI_ROOM_PRESENCE_UNKNOWN, HOME_AI_ROOM_PRESENCE_VACANT, 1U,
         "sensair_shuttle_01", "sensair_shuttle_01"},
        {HOME_AI_ROOM_PRESENCE_OCCUPIED, HOME_AI_ROOM_PRESENCE_UNKNOWN, 2U,
         "sensair_shuttle_01", "sensair_shuttle_02"},
        {HOME_AI_ROOM_PRESENCE_VACANT, HOME_AI_ROOM_PRESENCE_UNKNOWN, 1U,
         "sensair_shuttle_02", "sensair_shuttle_02"},
    };
    for (size_t index = 0U; index < sizeof(cases) / sizeof(cases[0]); ++index) {
        reset_fixture();
        s_states[RADAR_SOURCE_C51].presence_state = cases[index].c51;
        s_states[RADAR_SOURCE_C52].presence_state = cases[index].c52;
        s_session_busy = true;
        home_ai_voice_route_result_t route = {0};
        assert(home_ai_voice_router_request_prompt("living_room",
                                                   "emergency",
                                                   "decision_emergency",
                                                   true,
                                                   1000U,
                                                   &route) == ESP_OK);
        assert(route.queued_count == cases[index].expected_count);
        assert(s_queue_count == cases[index].expected_count);
        if (cases[index].expected_count == 0U) {
            assert(route.status == HOME_AI_VOICE_ROUTE_SUPPRESSED_NO_TERMINAL);
            assert(s_session_busy);
        } else {
            assert(route.status == HOME_AI_VOICE_ROUTE_QUEUED);
            assert(!s_session_busy);
            assert(strcmp(s_targets[0], cases[index].first_target) == 0);
            assert(strcmp(s_targets[s_queue_count - 1U], cases[index].last_target) == 0);
        }
    }
}

static void test_enqueue_failure_and_pending_ack_count(void)
{
    reset_fixture();
    s_fail_all_enqueues = true;
    home_ai_voice_route_result_t route = {0};
    assert(home_ai_voice_router_request_prompt("bedroom_01",
                                               "queue full",
                                               "decision_fail",
                                               false,
                                               1000U,
                                               &route) == ESP_ERR_NO_MEM);
    assert(route.status == HOME_AI_VOICE_ROUTE_REJECTED_RESOURCE);
    assert(route.playback_generation == 0U);
    s_fail_all_enqueues = false;
    assert(home_ai_voice_router_request_prompt("bedroom_01",
                                               "queue recovered",
                                               "decision_recovered",
                                               false,
                                               1000U,
                                               &route) == ESP_OK);
    assert(route.status == HOME_AI_VOICE_ROUTE_QUEUED);

    reset_fixture();
    s_states[RADAR_SOURCE_C52].presence_state = HOME_AI_ROOM_PRESENCE_OCCUPIED;
    s_fail_enqueue_attempt = 2U;
    assert(home_ai_voice_router_request_prompt("living_room",
                                               "partial emergency",
                                               "decision_partial",
                                               true,
                                               1000U,
                                               &route) == ESP_OK);
    assert(route.status == HOME_AI_VOICE_ROUTE_QUEUED);
    assert(route.queued_count == 1U);
    assert(home_ai_voice_router_acknowledge(route.playback_generation));
    assert(!home_ai_voice_router_acknowledge(route.playback_generation));

    reset_fixture();
    s_states[RADAR_SOURCE_C52].presence_state = HOME_AI_ROOM_PRESENCE_OCCUPIED;
    assert(home_ai_voice_router_request_prompt("living_room",
                                               "two terminal emergency",
                                               "decision_two",
                                               true,
                                               1000U,
                                               &route) == ESP_OK);
    assert(route.queued_count == 2U);
    assert(home_ai_voice_router_acknowledge(route.playback_generation));
    assert(home_ai_voice_router_acknowledge(route.playback_generation));
    assert(!home_ai_voice_router_acknowledge(route.playback_generation));
}

static void test_inflight_budget_ack_and_timeout(void)
{
    reset_fixture();
    uint32_t generations[HOME_AI_VOICE_ROUTER_MAX_INFLIGHT] = {0};
    home_ai_voice_route_result_t route = {0};
    for (size_t index = 0U; index < HOME_AI_VOICE_ROUTER_MAX_INFLIGHT; ++index) {
        assert(home_ai_voice_router_request_prompt("living_room",
                                                   "budget emergency",
                                                   "decision_budget",
                                                   true,
                                                   1000U,
                                                   &route) == ESP_OK);
        assert(route.status == HOME_AI_VOICE_ROUTE_QUEUED);
        generations[index] = route.playback_generation;
    }
    assert(home_ai_voice_router_request_prompt("living_room",
                                               "budget exhausted",
                                               "decision_exhausted",
                                               true,
                                               1000U,
                                               &route) == ESP_ERR_NO_MEM);
    assert(route.status == HOME_AI_VOICE_ROUTE_REJECTED_RESOURCE);
    assert(home_ai_voice_router_acknowledge(generations[0]));
    assert(!home_ai_voice_router_acknowledge(generations[0]));
    assert(home_ai_voice_router_request_prompt("living_room",
                                               "ack released",
                                               "decision_ack_release",
                                               true,
                                               1000U,
                                               &route) == ESP_OK);
    assert(route.status == HOME_AI_VOICE_ROUTE_QUEUED);

    home_ai_voice_router_tick(31000U);
    assert(home_ai_voice_router_request_prompt("living_room",
                                               "timeout released",
                                               "decision_timeout_release",
                                               true,
                                               31000U,
                                               &route) == ESP_OK);
    assert(route.status == HOME_AI_VOICE_ROUTE_QUEUED);
}

int main(void)
{
    test_stop_routes_to_bound_terminal();
    test_normal_routing_and_policy();
    test_dynamic_terminal_binding_and_empty_terminal();
    test_emergency_matrix();
    test_enqueue_failure_and_pending_ack_count();
    test_inflight_budget_ack_and_timeout();

    puts("home ai voice router host tests: PASS");
    return 0;
}
