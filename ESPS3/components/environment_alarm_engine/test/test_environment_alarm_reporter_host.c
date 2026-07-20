#include <assert.h>
#include <stdio.h>

#include "environment_alarm_engine.h"
#include "environment_alarm_reporter.h"

static uint64_t s_now_ms;

static uint64_t test_clock(void *context)
{
    (void)context;
    return s_now_ms;
}

static alarm_environment_sample_t sample(uint64_t sequence, float temperature)
{
    return (alarm_environment_sample_t){
        .struct_version = ALARM_ENVIRONMENT_SAMPLE_VERSION,
        .device_id = ALARM_DEVICE_C51,
        .ingest_seq = sequence,
        .local_ingest_seq = sequence,
        .remote_seq = (uint32_t)sequence,
        .valid_fields = ALARM_FIELD_TEMPERATURE | ALARM_FIELD_HUMIDITY |
                        ALARM_FIELD_AIR_QUALITY_SCORE | ALARM_FIELD_GAS_RATIO |
                        ALARM_FIELD_STABILITY_SCORE,
        .temperature = temperature,
        .humidity = 50.0f,
        .air_quality_score = 80.0f,
        .air_quality_level = ALARM_AIR_QUALITY_GOOD,
        .gas_ratio = 1.0f,
        .stability_score = 0.95f,
        .sensor_state = ALARM_SENSOR_READY,
    };
}

static uint64_t s_sequence;
static size_t s_observer_count;
static size_t s_observer_engine_depth;
static alarm_event_t s_observed_event;

static void observe_alarm(const alarm_event_t *event, void *context)
{
    (void)context;
    assert(event != NULL);
    s_observed_event = *event;
    s_observer_engine_depth = alarm_engine_get_queue_depth();
    ++s_observer_count;
}

static void emit_temperature_samples(float temperature,
                                     uint64_t first_time_ms,
                                     uint64_t interval_ms,
                                     unsigned int count)
{
    for (unsigned int index = 0U; index < count; ++index) {
        ++s_sequence;
        s_now_ms = first_time_ms + (uint64_t)index * interval_ms;
        alarm_environment_sample_t item = sample(s_sequence, temperature);
        assert(alarm_engine_update(&item, NULL) == ESP_OK);
    }
}

int main(void)
{
    s_now_ms = 0U;
    s_sequence = 0U;
    assert(alarm_engine_init(&(alarm_engine_options_t){.monotonic_clock_ms = test_clock}) == ESP_OK);

    emit_temperature_samples(36.0f, 0U, 30000U, 3U);
    assert(alarm_engine_get_queue_depth() == 1U);
    assert(environment_alarm_reporter_drain_engine() == ESP_ERR_INVALID_STATE);
    assert(alarm_engine_get_queue_depth() == 1U);

    assert(environment_alarm_reporter_init() == ESP_OK);
    assert(environment_alarm_reporter_set_observer(observe_alarm, NULL) == ESP_OK);
    assert(environment_alarm_reporter_drain_engine() == ESP_OK);
    assert(s_observer_count == 1U);
    assert(s_observer_engine_depth == 1U);
    assert(s_observed_event.event_seq != 0U);
    assert(s_observed_event.alarm_type == ALARM_TYPE_HIGH_TEMPERATURE);
    assert(alarm_engine_get_queue_depth() == 0U);

    environment_alarm_reporter_stats_t stats = {0};
    assert(environment_alarm_reporter_get_stats(&stats) == ESP_OK);
    assert(stats.events_enqueued == 1U);
    assert(stats.report_queue_depth == 1U);

    uint64_t next_time = 90000U;
    bool queue_full_observed = false;
    for (unsigned int cycle = 0U; cycle < 20U && !queue_full_observed; ++cycle) {
        emit_temperature_samples(32.0f, next_time, 30000U, 5U);
        next_time += 150000U;
        const esp_err_t recovery_drain = environment_alarm_reporter_drain_engine();
        if (recovery_drain == ESP_ERR_NO_MEM) {
            queue_full_observed = true;
            break;
        }
        assert(recovery_drain == ESP_OK);
        emit_temperature_samples(36.0f, next_time, 30000U, 3U);
        next_time += 90000U;
        const esp_err_t drain = environment_alarm_reporter_drain_engine();
        if (drain == ESP_ERR_NO_MEM) {
            queue_full_observed = true;
        } else {
            assert(drain == ESP_OK);
        }
    }
    assert(queue_full_observed);
    assert(alarm_engine_get_queue_depth() > 0U);
    assert(alarm_engine_ack_events(UINT64_MAX) == ESP_ERR_INVALID_ARG);
    assert(environment_alarm_reporter_get_stats(&stats) == ESP_OK);
    assert(stats.queue_full > 0U);
    puts("environment_alarm_reporter_host: PASS");
    return 0;
}
