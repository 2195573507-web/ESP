#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "gateway_config.h"
#include "home_ai_event_reporter.h"

static home_ai_history_stats_t s_stats;
static home_ai_history_event_t s_last_event;
static unsigned int s_event_count;
static char s_last_network_body[1536];
static gateway_runtime_config_t s_gateway_config = {.gateway_id = "test_gateway"};

const gateway_runtime_config_t *gateway_config_get(void)
{
    return &s_gateway_config;
}

bool home_ai_history_store_init(void)
{
    return true;
}

home_ai_history_stats_t home_ai_history_get_stats(void)
{
    return s_stats;
}

esp_err_t home_ai_history_enqueue(const home_ai_history_event_t *event)
{
    assert(event != NULL);
    s_last_event = *event;
    ++s_event_count;
    return ESP_OK;
}

esp_err_t network_worker_submit_home_ai_events(char *json_body, const char *source)
{
    assert(json_body != NULL);
    assert(strcmp(source, "home_ai_event") == 0);
    snprintf(s_last_network_body, sizeof(s_last_network_body), "%s", json_body);
    return ESP_OK;
}

void network_replay_worker_request_home_ai_replay(void)
{
}

int main(void)
{
    memset(&s_stats, 0, sizeof(s_stats));
    memset(&s_last_event, 0, sizeof(s_last_event));
    s_event_count = 0U;
    assert(home_ai_event_reporter_init());
    home_ai_event_reporter_tick(1000U);
    assert(s_event_count == 0U);

    s_stats.dropped_unpersisted = 1U;
    home_ai_event_reporter_tick(1000U);
    assert(s_event_count == 1U);
    assert(strcmp(s_last_event.event_type, "offline_buffer") == 0);
    assert(strstr(s_last_event.payload, "\"dropped_unpersisted\":1") != NULL);

    s_stats.dropped_overwrite = 2U;
    home_ai_event_reporter_tick(2000U);
    assert(s_event_count == 1U);
    home_ai_event_reporter_tick(61000U);
    assert(s_event_count == 2U);
    assert(strstr(s_last_event.payload, "\"dropped_overwrite\":2") != NULL);

    s_stats.storage_errors = 3U;
    home_ai_event_reporter_tick(61001U);
    assert(s_event_count == 2U);
    home_ai_event_reporter_tick(121000U);
    assert(s_event_count == 3U);
    assert(strstr(s_last_network_body, "\"storage_errors\":3") != NULL);

    s_stats.protected_rejections = 4U;
    home_ai_event_reporter_tick(121001U);
    assert(s_event_count == 3U);
    home_ai_event_reporter_tick(181000U);
    assert(s_event_count == 4U);
    assert(strstr(s_last_event.payload, "\"protected_rejections\":4") != NULL);
    home_ai_event_reporter_tick(241000U);
    assert(s_event_count == 4U);

    puts("home ai event reporter host tests: PASS");
    return 0;
}
