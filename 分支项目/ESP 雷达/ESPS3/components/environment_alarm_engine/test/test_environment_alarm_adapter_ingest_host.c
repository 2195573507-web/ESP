#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "environment_alarm_adapter.h"
#include "environment_alarm_engine.h"
#include "esp111_protocol_common.h"

void environment_alarm_test_set_time_ms(uint64_t time_ms);

static cJSON *make_payload(float temperature,
                           const char *state,
                           uint32_t boot_id,
                           bool include_stability)
{
    cJSON *payload = cJSON_CreateObject();
    cJSON *air_quality = cJSON_AddObjectToObject(payload, "air_quality");
    assert(payload != NULL && air_quality != NULL);
    cJSON_AddNumberToObject(payload, "temperature_c", temperature);
    cJSON_AddNumberToObject(payload, "humidity_percent", 50.0);
    cJSON_AddNumberToObject(payload, "pressure_hpa", 1000.0);
    cJSON_AddNumberToObject(payload, "gas_resistance_ohm", 10000.0);
    cJSON_AddNumberToObject(payload, "boot_id", boot_id);
    cJSON_AddBoolToObject(payload, "time_synced", false);
    cJSON_AddNumberToObject(air_quality, "score", 80.0);
    cJSON_AddStringToObject(air_quality, "level", "good");
    cJSON_AddNumberToObject(air_quality, "gas_ratio", 1.0);
    if (include_stability) {
        cJSON_AddNumberToObject(air_quality, "stability_score", 0.95);
    }
    cJSON_AddStringToObject(air_quality, "sensor_state", state);
    return payload;
}

static void ingest(cJSON *payload, const char *device_id, uint32_t sequence, uint64_t time_ms)
{
    protocol_adapter_envelope_t envelope = {0};
    environment_alarm_test_set_time_ms(time_ms);
    strcpy(envelope.message_type, ESP111_PROTOCOL_MSG_SENSOR_BME690);
    strcpy(envelope.device_id, device_id);
    strcpy(envelope.room_id, "host_room");
    envelope.seq = sequence;
    envelope.payload = payload;
    assert(environment_alarm_adapter_ingest(&envelope) == ESP_OK);
    cJSON_Delete(payload);
}

int main(void)
{
    assert(environment_alarm_adapter_init() == ESP_OK);

    ingest(make_payload(36.0f, "READY", 100U, true),
           ESP111_PROTOCOL_TERMINAL_DEVICE_ID_C51, 1U, 0U);
    ingest(make_payload(36.0f, "READY", 100U, true),
           ESP111_PROTOCOL_TERMINAL_DEVICE_ID_C51, 2U, 30000U);
    ingest(make_payload(36.0f, "READY", 100U, true),
           ESP111_PROTOCOL_TERMINAL_DEVICE_ID_C51, 3U, 60000U);
    assert(alarm_engine_get_active(ALARM_DEVICE_C51, &(alarm_active_alarm_t){0}, 1U) == 1U);

    ingest(make_payload(22.0f, "READY", 101U, true),
           ESP111_PROTOCOL_TERMINAL_DEVICE_ID_C51, 1U, 90000U);
    assert(alarm_engine_get_active(ALARM_DEVICE_C51, &(alarm_active_alarm_t){0}, 1U) == 0U);

    ingest(make_payload(22.0f, "READY", 101U, true),
           ESP111_PROTOCOL_TERMINAL_DEVICE_ID_C51, 1U, 120000U);
    ingest(make_payload(22.0f, "READY", 101U, true),
           ESP111_PROTOCOL_TERMINAL_DEVICE_ID_C51, 0U, 150000U);
    ingest(make_payload(22.0f, "READY", 102U, true),
           ESP111_PROTOCOL_TERMINAL_DEVICE_ID_C51, UINT32_MAX - 1U, 180000U);
    ingest(make_payload(22.0f, "READY", 102U, true),
           ESP111_PROTOCOL_TERMINAL_DEVICE_ID_C51, 1U, 210000U);

    ingest(make_payload(36.0f, "WARMUP", 200U, false),
           ESP111_PROTOCOL_TERMINAL_DEVICE_ID_C52, 1U, 0U);
    ingest(make_payload(36.0f, "WARMUP", 200U, false),
           ESP111_PROTOCOL_TERMINAL_DEVICE_ID_C52, 2U, 30000U);
    ingest(make_payload(36.0f, "WARMUP", 200U, false),
           ESP111_PROTOCOL_TERMINAL_DEVICE_ID_C52, 3U, 60000U);
    assert(alarm_engine_get_active(ALARM_DEVICE_C52, &(alarm_active_alarm_t){0}, 1U) == 1U);

    environment_alarm_adapter_stats_t stats = {0};
    assert(environment_alarm_adapter_get_stats(&stats) == ESP_OK);
    assert(stats.restarts == 2U);
    assert(stats.duplicates >= 1U);
    assert(stats.out_of_order >= 1U);
    assert(stats.sequence_wraps >= 1U);
    assert(stats.missing_field >= 1U);
    puts("environment_alarm_adapter_ingest_host: PASS");
    return 0;
}
