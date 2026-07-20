#include <stdint.h>

#include "environment_alarm_reporter.h"
#include "protocol_adapter.h"

static int64_t s_time_us;

int64_t environment_alarm_test_time_us(void)
{
    return s_time_us;
}

void environment_alarm_test_set_time_ms(uint64_t time_ms)
{
    s_time_us = (int64_t)(time_ms * 1000U);
}

protocol_adapter_message_kind_t protocol_adapter_message_kind(const char *message_type)
{
    (void)message_type;
    return PROTOCOL_ADAPTER_MESSAGE_SENSOR_BME690;
}

esp_err_t environment_alarm_reporter_init(void)
{
    return ESP_OK;
}

esp_err_t environment_alarm_reporter_drain_engine(void)
{
    return ESP_OK;
}

esp_err_t environment_alarm_reporter_get_stats(environment_alarm_reporter_stats_t *out)
{
    (void)out;
    return ESP_ERR_NOT_SUPPORTED;
}
