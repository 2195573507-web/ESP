#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "environment_alarm_engine.h"

static uint64_t s_now_ms;
static uint64_t s_next_sequence[ALARM_DEVICE_INVALID];

static uint64_t test_clock(void *context)
{
    (void)context;
    return s_now_ms;
}

static void init_engine(void)
{
    s_now_ms = 0U;
    memset(s_next_sequence, 0, sizeof(s_next_sequence));
    alarm_engine_deinit();
    assert(alarm_engine_init(&(alarm_engine_options_t){.monotonic_clock_ms = test_clock}) == ESP_OK);
}

static alarm_environment_sample_t make_sample(alarm_device_id_t device)
{
    return (alarm_environment_sample_t){
        .struct_version = ALARM_ENVIRONMENT_SAMPLE_VERSION,
        .device_id = device,
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
}

static void send_sample(alarm_environment_sample_t *sample, uint64_t now_ms)
{
    assert(sample != NULL);
    assert(sample->device_id < ALARM_DEVICE_INVALID);
    const uint64_t sequence = ++s_next_sequence[sample->device_id];
    sample->ingest_seq = sequence;
    sample->local_ingest_seq = sequence;
    sample->remote_seq = (uint32_t)sequence;
    s_now_ms = now_ms;
    assert(alarm_engine_update(sample, NULL) == ESP_OK);
}

static size_t copy_events(alarm_event_t *events, size_t capacity)
{
    memset(events, 0, capacity * sizeof(*events));
    return alarm_engine_peek_events(events, capacity);
}

static bool has_event(const alarm_event_t *events,
                      size_t count,
                      alarm_type_t type,
                      alarm_event_status_t status)
{
    for (size_t index = 0U; index < count; ++index) {
        if (events[index].alarm_type == type && events[index].status == status) {
            return true;
        }
    }
    return false;
}

static uint64_t event_alarm_id(const alarm_event_t *events,
                               size_t count,
                               alarm_type_t type,
                               alarm_event_status_t status)
{
    for (size_t index = 0U; index < count; ++index) {
        if (events[index].alarm_type == type && events[index].status == status) {
            return events[index].alarm_id;
        }
    }
    return 0U;
}

static void test_humidity_activation_and_recovery(void)
{
    init_engine();
    alarm_environment_sample_t sample = make_sample(ALARM_DEVICE_C51);
    sample.humidity = 80.0f;
    send_sample(&sample, 0U);
    send_sample(&sample, 30000U);
    send_sample(&sample, 60000U);

    alarm_event_t events[ALARM_ENGINE_MAX_EVENTS];
    size_t count = copy_events(events, ALARM_ENGINE_MAX_EVENTS);
    assert(has_event(events, count, ALARM_TYPE_HIGH_HUMIDITY, ALARM_STATUS_ACTIVE));

    sample.humidity = 65.0f;
    send_sample(&sample, 90000U);
    send_sample(&sample, 120000U);
    send_sample(&sample, 150000U);
    send_sample(&sample, 180000U);
    send_sample(&sample, 210000U);
    count = copy_events(events, ALARM_ENGINE_MAX_EVENTS);
    assert(has_event(events, count, ALARM_TYPE_HIGH_HUMIDITY, ALARM_STATUS_RECOVERED));
}

static void test_air_quality_activation_and_recovery(void)
{
    init_engine();
    alarm_environment_sample_t sample = make_sample(ALARM_DEVICE_C51);
    sample.air_quality_score = 20.0f;
    sample.air_quality_level = ALARM_AIR_QUALITY_BAD;
    send_sample(&sample, 0U);
    send_sample(&sample, 30000U);

    alarm_event_t events[ALARM_ENGINE_MAX_EVENTS];
    size_t count = copy_events(events, ALARM_ENGINE_MAX_EVENTS);
    assert(has_event(events, count, ALARM_TYPE_AIR_QUALITY_CRITICAL, ALARM_STATUS_ACTIVE));
    assert(!has_event(events, count, ALARM_TYPE_AIR_QUALITY_WARNING, ALARM_STATUS_ACTIVE));

    sample.air_quality_score = 80.0f;
    sample.air_quality_level = ALARM_AIR_QUALITY_GOOD;
    send_sample(&sample, 60000U);
    send_sample(&sample, 90000U);
    send_sample(&sample, 120000U);
    send_sample(&sample, 150000U);
    send_sample(&sample, 180000U);
    count = copy_events(events, ALARM_ENGINE_MAX_EVENTS);
    assert(has_event(events, count, ALARM_TYPE_AIR_QUALITY_CRITICAL, ALARM_STATUS_RECOVERED));
}

static void test_pollution_escalation_and_recovery(void)
{
    init_engine();
    alarm_environment_sample_t sample = make_sample(ALARM_DEVICE_C51);
    sample.gas_ratio = 0.60f;
    send_sample(&sample, 0U);
    send_sample(&sample, 1000U);
    send_sample(&sample, 2000U);
    sample.gas_ratio = 0.50f;
    send_sample(&sample, 3000U);
    send_sample(&sample, 4000U);

    alarm_event_t events[ALARM_ENGINE_MAX_EVENTS];
    size_t count = copy_events(events, ALARM_ENGINE_MAX_EVENTS);
    bool saw_high = false;
    bool saw_critical = false;
    for (size_t index = 0U; index < count; ++index) {
        if (events[index].alarm_type != ALARM_TYPE_POLLUTION_SPIKE ||
            events[index].status != ALARM_STATUS_ACTIVE) {
            continue;
        }
        saw_high = saw_high || events[index].alarm_level == ALARM_LEVEL_HIGH;
        saw_critical = saw_critical || events[index].alarm_level == ALARM_LEVEL_CRITICAL;
    }
    assert(saw_high && saw_critical);

    sample.gas_ratio = 0.90f;
    send_sample(&sample, 5000U);
    send_sample(&sample, 35000U);
    send_sample(&sample, 65000U);
    send_sample(&sample, 95000U);
    send_sample(&sample, 125000U);
    count = copy_events(events, ALARM_ENGINE_MAX_EVENTS);
    assert(has_event(events, count, ALARM_TYPE_POLLUTION_SPIKE, ALARM_STATUS_RECOVERED));
}

static void test_critical_environment_and_unknown_gate(void)
{
    init_engine();
    alarm_environment_sample_t sample = make_sample(ALARM_DEVICE_C51);
    sample.temperature = 42.0f;
    sample.air_quality_score = 20.0f;
    sample.air_quality_level = ALARM_AIR_QUALITY_BAD;
    send_sample(&sample, 0U);
    send_sample(&sample, 30000U);
    send_sample(&sample, 60000U);
    send_sample(&sample, 90000U);
    send_sample(&sample, 120000U);

    alarm_event_t events[ALARM_ENGINE_MAX_EVENTS];
    size_t count = copy_events(events, ALARM_ENGINE_MAX_EVENTS);
    assert(has_event(events, count, ALARM_TYPE_CRITICAL_ENVIRONMENT, ALARM_STATUS_ACTIVE));

    init_engine();
    sample = make_sample(ALARM_DEVICE_C51);
    sample.temperature = 36.0f;
    sample.air_quality_score = 10.0f;
    sample.air_quality_level = ALARM_AIR_QUALITY_BAD;
    sample.sensor_state = ALARM_SENSOR_UNKNOWN;
    send_sample(&sample, 0U);
    send_sample(&sample, 30000U);
    send_sample(&sample, 60000U);
    count = copy_events(events, ALARM_ENGINE_MAX_EVENTS);
    assert(has_event(events, count, ALARM_TYPE_HIGH_TEMPERATURE, ALARM_STATUS_ACTIVE));
    assert(!has_event(events, count, ALARM_TYPE_AIR_QUALITY_WARNING, ALARM_STATUS_ACTIVE));
    assert(!has_event(events, count, ALARM_TYPE_AIR_QUALITY_CRITICAL, ALARM_STATUS_ACTIVE));
}

static void test_reset_isolation_and_alarm_ids(void)
{
    init_engine();
    alarm_environment_sample_t c51 = make_sample(ALARM_DEVICE_C51);
    alarm_environment_sample_t c52 = make_sample(ALARM_DEVICE_C52);
    c51.temperature = 36.0f;
    c52.humidity = 80.0f;
    for (uint64_t now_ms = 0U; now_ms <= 60000U; now_ms += 30000U) {
        send_sample(&c51, now_ms);
        send_sample(&c52, now_ms);
    }
    assert(alarm_engine_get_active(ALARM_DEVICE_C51, &(alarm_active_alarm_t){0}, 1U) == 1U);
    assert(alarm_engine_get_active(ALARM_DEVICE_C52, &(alarm_active_alarm_t){0}, 1U) == 1U);
    assert(alarm_engine_reset(false, ALARM_DEVICE_C51) == ESP_OK);
    assert(alarm_engine_get_active(ALARM_DEVICE_C51, &(alarm_active_alarm_t){0}, 1U) == 0U);
    assert(alarm_engine_get_active(ALARM_DEVICE_C52, &(alarm_active_alarm_t){0}, 1U) == 1U);

    init_engine();
    c51 = make_sample(ALARM_DEVICE_C51);
    c51.temperature = 36.0f;
    send_sample(&c51, 0U);
    send_sample(&c51, 30000U);
    send_sample(&c51, 60000U);
    alarm_event_t events[ALARM_ENGINE_MAX_EVENTS];
    size_t count = copy_events(events, ALARM_ENGINE_MAX_EVENTS);
    const uint64_t first_alarm_id = event_alarm_id(events,
                                                   count,
                                                   ALARM_TYPE_HIGH_TEMPERATURE,
                                                   ALARM_STATUS_ACTIVE);
    assert(first_alarm_id != 0U);
    c51.temperature = 32.0f;
    send_sample(&c51, 90000U);
    send_sample(&c51, 120000U);
    send_sample(&c51, 150000U);
    send_sample(&c51, 180000U);
    send_sample(&c51, 210000U);
    c51.temperature = 36.0f;
    send_sample(&c51, 240000U);
    send_sample(&c51, 270000U);
    send_sample(&c51, 300000U);
    count = copy_events(events, ALARM_ENGINE_MAX_EVENTS);
    uint64_t second_alarm_id = 0U;
    for (size_t index = 0U; index < count; ++index) {
        if (events[index].alarm_type == ALARM_TYPE_HIGH_TEMPERATURE &&
            events[index].status == ALARM_STATUS_ACTIVE &&
            events[index].alarm_id != first_alarm_id) {
            second_alarm_id = events[index].alarm_id;
        }
    }
    assert(second_alarm_id != 0U);
}

static void test_invalid_fields_and_timestamp_rollback(void)
{
    init_engine();
    alarm_environment_sample_t sample = make_sample(ALARM_DEVICE_C51);
    sample.temperature = NAN;
    sample.air_quality_score = INFINITY;
    sample.stability_score = 1.5f;
    send_sample(&sample, 0U);
    send_sample(&sample, 30000U);
    assert(alarm_engine_get_queue_depth() == 0U);

    init_engine();
    sample = make_sample(ALARM_DEVICE_C51);
    sample.temperature = 36.0f;
    sample.timestamp_valid = true;
    sample.timestamp_ms = 1000U;
    send_sample(&sample, 0U);
    sample.timestamp_ms = 500U;
    send_sample(&sample, 30000U);
    sample.timestamp_ms = 100U;
    send_sample(&sample, 60000U);
    alarm_event_t events[ALARM_ENGINE_MAX_EVENTS];
    const size_t count = copy_events(events, ALARM_ENGINE_MAX_EVENTS);
    assert(has_event(events, count, ALARM_TYPE_HIGH_TEMPERATURE, ALARM_STATUS_ACTIVE));
    assert(events[0].event_monotonic_ms == 60000U);
    assert(events[0].timestamp_ms == 100U);
}

static void test_remaining_rule_families(void)
{
    alarm_event_t events[ALARM_ENGINE_MAX_EVENTS];

    init_engine();
    alarm_environment_sample_t sample = make_sample(ALARM_DEVICE_C51);
    sample.temperature = 5.0f;
    sample.humidity = 10.0f;
    send_sample(&sample, 0U);
    send_sample(&sample, 30000U);
    send_sample(&sample, 60000U);
    size_t count = copy_events(events, ALARM_ENGINE_MAX_EVENTS);
    assert(has_event(events, count, ALARM_TYPE_LOW_TEMPERATURE, ALARM_STATUS_ACTIVE));
    assert(has_event(events, count, ALARM_TYPE_LOW_HUMIDITY, ALARM_STATUS_ACTIVE));

    init_engine();
    sample = make_sample(ALARM_DEVICE_C51);
    sample.temperature = 20.0f;
    sample.humidity = 50.0f;
    send_sample(&sample, 0U);
    sample.temperature = 24.0f;
    sample.humidity = 70.0f;
    send_sample(&sample, 10000U);
    send_sample(&sample, 20000U);
    count = copy_events(events, ALARM_ENGINE_MAX_EVENTS);
    assert(has_event(events, count, ALARM_TYPE_FAST_TEMPERATURE_CHANGE, ALARM_STATUS_ACTIVE));
    assert(has_event(events, count, ALARM_TYPE_FAST_HUMIDITY_CHANGE, ALARM_STATUS_ACTIVE));

    init_engine();
    sample = make_sample(ALARM_DEVICE_C51);
    sample.air_quality_score = 40.0f;
    sample.air_quality_level = ALARM_AIR_QUALITY_POOR;
    send_sample(&sample, 0U);
    send_sample(&sample, 30000U);
    send_sample(&sample, 60000U);
    count = copy_events(events, ALARM_ENGINE_MAX_EVENTS);
    assert(has_event(events, count, ALARM_TYPE_AIR_QUALITY_WARNING, ALARM_STATUS_ACTIVE));

    init_engine();
    sample = make_sample(ALARM_DEVICE_C51);
    sample.air_quality_score = 90.0f;
    send_sample(&sample, 0U);
    sample.air_quality_score = 60.0f;
    sample.gas_ratio = 0.80f;
    send_sample(&sample, 30000U);
    send_sample(&sample, 60000U);
    send_sample(&sample, 90000U);
    count = copy_events(events, ALARM_ENGINE_MAX_EVENTS);
    assert(has_event(events, count, ALARM_TYPE_AIR_QUALITY_DETERIORATING, ALARM_STATUS_ACTIVE));

    init_engine();
    sample = make_sample(ALARM_DEVICE_C51);
    sample.stability_score = 0.40f;
    send_sample(&sample, 0U);
    send_sample(&sample, 1000U);
    send_sample(&sample, 2000U);
    count = copy_events(events, ALARM_ENGINE_MAX_EVENTS);
    assert(has_event(events, count, ALARM_TYPE_ENVIRONMENT_UNSTABLE, ALARM_STATUS_ACTIVE));
}

int main(void)
{
    test_humidity_activation_and_recovery();
    test_air_quality_activation_and_recovery();
    test_pollution_escalation_and_recovery();
    test_critical_environment_and_unknown_gate();
    test_reset_isolation_and_alarm_ids();
    test_invalid_fields_and_timestamp_rollback();
    test_remaining_rule_families();
    puts("environment_alarm_acceptance_host: PASS");
    return 0;
}
