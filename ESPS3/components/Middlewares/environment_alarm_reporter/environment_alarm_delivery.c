#include "environment_alarm_delivery.h"

#define ENV_ALARM_RETRY_BASE_MS 1000U
#define ENV_ALARM_RETRY_MAX_MS 60000U

environment_alarm_delivery_outcome_t environment_alarm_delivery_classify(esp_err_t result,
                                                                           int http_status)
{
    if (result == ESP_OK && http_status >= 200 && http_status < 300) {
        return ENVIRONMENT_ALARM_DELIVERY_SENT;
    }
    if (http_status >= 400 && http_status < 500 && http_status != 408 && http_status != 429) {
        return ENVIRONMENT_ALARM_DELIVERY_DEAD_LETTER;
    }
    if (http_status == 0 &&
        (result == ESP_ERR_INVALID_ARG || result == ESP_ERR_INVALID_SIZE ||
         result == ESP_ERR_NOT_ALLOWED)) {
        return ENVIRONMENT_ALARM_DELIVERY_DEAD_LETTER;
    }
    return ENVIRONMENT_ALARM_DELIVERY_RETRY;
}

uint32_t environment_alarm_delivery_retry_delay_ms(uint8_t retry_count)
{
    const uint8_t shift = retry_count > 6U ? 6U : retry_count;
    const uint32_t delay_ms = ENV_ALARM_RETRY_BASE_MS << shift;
    return delay_ms > ENV_ALARM_RETRY_MAX_MS ? ENV_ALARM_RETRY_MAX_MS : delay_ms;
}
