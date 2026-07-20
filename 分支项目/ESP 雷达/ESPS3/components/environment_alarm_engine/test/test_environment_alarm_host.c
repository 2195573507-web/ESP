#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "environment_alarm_engine.h"

static uint64_t s_now_ms;

static uint64_t test_clock(void *context)
{
    (void)context;
    return s_now_ms;
}

static alarm_environment_sample_t make_sample(alarm_device_id_t device, uint64_t sequence)
{
    alarm_environment_sample_t sample = {
        .struct_version = ALARM_ENVIRONMENT_SAMPLE_VERSION,
        .device_id = device,
        .ingest_seq = sequence,
        .local_ingest_seq = sequence,
        .remote_seq = (uint32_t)sequence,
        .valid_fields = ALARM_FIELD_TEMPERATURE | ALARM_FIELD_HUMIDITY |
                        ALARM_FIELD_PRESSURE | ALARM_FIELD_GAS_RESISTANCE |
                        ALARM_FIELD_AIR_QUALITY_SCORE | ALARM_FIELD_AIR_QUALITY_LEVEL |
                        ALARM_FIELD_GAS_RATIO | ALARM_FIELD_STABILITY_SCORE,
        .temperature = 22.0f,
        .humidity = 50.0f,
        .pressure = 1000.0f,
        .gas_resistance = 10000.0f,
        .air_quality_score = 80.0f,
        .air_quality_level = ALARM_AIR_QUALITY_GOOD,
        .gas_ratio = 1.0f,
        .stability_score = 0.95f,
        .sensor_state = ALARM_SENSOR_READY,
    };
    strcpy(sample.room_id, device == ALARM_DEVICE_C52 ? "bedroom" : "living_room");
    strcpy(sample.source, "host_test");
    return sample;
}

static void init_engine(void)
{
    s_now_ms = 0U;
    alarm_engine_deinit();
    assert(alarm_engine_init(&(alarm_engine_options_t){.monotonic_clock_ms = test_clock}) == ESP_OK);
}

static void update(alarm_environment_sample_t *sample, uint64_t now)
{
    s_now_ms = now;
    assert(alarm_engine_update(sample, NULL) == ESP_OK);
}

static void test_temperature_hysteresis_and_recovery(void)
{
    init_engine();
    alarm_environment_sample_t sample = make_sample(ALARM_DEVICE_C51, 1U);
    sample.temperature = 36.0f;
    update(&sample, 0U);
    sample.ingest_seq = sample.local_ingest_seq = 2U;
    update(&sample, 30000U);
    sample.ingest_seq = sample.local_ingest_seq = 3U;
    update(&sample, 60000U);

    alarm_event_t events[4] = {0};
    assert(alarm_engine_peek_events(events, 4U) == 1U);
    assert(events[0].alarm_type == ALARM_TYPE_HIGH_TEMPERATURE);
    assert(events[0].status == ALARM_STATUS_ACTIVE);
    assert(events[0].trigger_threshold == 35.0f);
    assert(strcmp(events[0].unit, "C") == 0);

    sample.temperature = 32.0f;
    sample.ingest_seq = sample.local_ingest_seq = 4U;
    update(&sample, 90000U);
    sample.ingest_seq = sample.local_ingest_seq = 5U;
    update(&sample, 120000U);
    sample.ingest_seq = sample.local_ingest_seq = 6U;
    update(&sample, 150000U);
    sample.ingest_seq = sample.local_ingest_seq = 7U;
    update(&sample, 180000U);
    sample.ingest_seq = sample.local_ingest_seq = 8U;
    update(&sample, 210000U);
    const size_t event_count = alarm_engine_peek_events(events, 4U);
    assert(event_count >= 2U);
    bool recovered = false;
    for (size_t index = 0U; index < event_count; ++index) {
        if (events[index].alarm_type == ALARM_TYPE_HIGH_TEMPERATURE &&
            events[index].status == ALARM_STATUS_RECOVERED) {
            assert(events[index].alarm_id == events[0].alarm_id);
            recovered = true;
        }
    }
    assert(recovered);
}

static void test_warmup_gates_air_quality_but_not_temperature(void)
{
    init_engine();
    alarm_environment_sample_t sample = make_sample(ALARM_DEVICE_C51, 1U);
    sample.sensor_state = ALARM_SENSOR_WARMUP;
    sample.air_quality_score = 10.0f;
    sample.air_quality_level = ALARM_AIR_QUALITY_BAD;
    sample.temperature = 36.0f;
    update(&sample, 0U);
    sample.ingest_seq = sample.local_ingest_seq = 2U;
    update(&sample, 30000U);
    sample.ingest_seq = sample.local_ingest_seq = 3U;
    update(&sample, 60000U);

    alarm_event_t events[4] = {0};
    assert(alarm_engine_peek_events(events, 4U) == 1U);
    assert(events[0].alarm_type == ALARM_TYPE_HIGH_TEMPERATURE);
}

static void test_degraded_and_device_isolation(void)
{
    init_engine();
    alarm_environment_sample_t c51 = make_sample(ALARM_DEVICE_C51, 1U);
    alarm_environment_sample_t c52 = make_sample(ALARM_DEVICE_C52, 1U);
    c51.sensor_state = ALARM_SENSOR_DEGRADED;
    update(&c51, 0U);
    c51.ingest_seq = c51.local_ingest_seq = 2U;
    update(&c51, 1000U);
    c51.ingest_seq = c51.local_ingest_seq = 3U;
    update(&c51, 2000U);
    update(&c52, 2000U);

    alarm_event_t events[4] = {0};
    assert(alarm_engine_peek_events(events, 4U) == 1U);
    assert(events[0].device_id == ALARM_DEVICE_C51);
    assert(events[0].alarm_type == ALARM_TYPE_SENSOR_DEGRADED);

    c51.sensor_state = ALARM_SENSOR_READY;
    c51.ingest_seq = c51.local_ingest_seq = 4U;
    update(&c51, 3000U);
    c51.ingest_seq = c51.local_ingest_seq = 5U;
    update(&c51, 4000U);
    c51.ingest_seq = c51.local_ingest_seq = 6U;
    update(&c51, 5000U);
    assert(alarm_engine_peek_events(events, 4U) == 2U);
    assert(events[1].status == ALARM_STATUS_RECOVERED);
}

static void test_invalid_and_missing_fields_are_gated(void)
{
    init_engine();
    alarm_environment_sample_t sample = make_sample(ALARM_DEVICE_C51, 1U);
    sample.temperature = NAN;
    sample.valid_fields &= ~ALARM_FIELD_TEMPERATURE;
    sample.air_quality_score = INFINITY;
    sample.valid_fields &= ~ALARM_FIELD_AIR_QUALITY_SCORE;
    update(&sample, 0U);
    sample.ingest_seq = sample.local_ingest_seq = 2U;
    update(&sample, 60000U);
    assert(alarm_engine_get_queue_depth() == 0U);
}

int main(void)
{
    test_temperature_hysteresis_and_recovery();
    test_warmup_gates_air_quality_but_not_temperature();
    test_degraded_and_device_isolation();
    test_invalid_and_missing_fields_are_gated();
    puts("environment_alarm_engine_host: PASS");
    return 0;
}
