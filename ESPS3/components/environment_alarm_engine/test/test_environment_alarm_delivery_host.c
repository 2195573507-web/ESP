#include <assert.h>
#include <stdio.h>

#include "environment_alarm_delivery.h"

static void assert_outcome(esp_err_t result,
                           int http_status,
                           environment_alarm_delivery_outcome_t expected)
{
    assert(environment_alarm_delivery_classify(result, http_status) == expected);
}

int main(void)
{
    assert_outcome(ESP_OK, 200, ENVIRONMENT_ALARM_DELIVERY_SENT);
    assert_outcome(ESP_OK, 201, ENVIRONMENT_ALARM_DELIVERY_SENT);
    assert_outcome(ESP_OK, 204, ENVIRONMENT_ALARM_DELIVERY_SENT);

    assert_outcome(ESP_ERR_TIMEOUT, 0, ENVIRONMENT_ALARM_DELIVERY_RETRY);
    assert_outcome(ESP_ERR_INVALID_RESPONSE, 408, ENVIRONMENT_ALARM_DELIVERY_RETRY);
    assert_outcome(ESP_ERR_INVALID_RESPONSE, 429, ENVIRONMENT_ALARM_DELIVERY_RETRY);
    assert_outcome(ESP_ERR_INVALID_RESPONSE, 500, ENVIRONMENT_ALARM_DELIVERY_RETRY);
    assert_outcome(ESP_ERR_INVALID_RESPONSE, 503, ENVIRONMENT_ALARM_DELIVERY_RETRY);

    assert_outcome(ESP_ERR_INVALID_RESPONSE, 400, ENVIRONMENT_ALARM_DELIVERY_DEAD_LETTER);
    assert_outcome(ESP_ERR_INVALID_RESPONSE, 401, ENVIRONMENT_ALARM_DELIVERY_DEAD_LETTER);
    assert_outcome(ESP_ERR_INVALID_ARG, 0, ENVIRONMENT_ALARM_DELIVERY_DEAD_LETTER);
    assert_outcome(ESP_ERR_INVALID_SIZE, 0, ENVIRONMENT_ALARM_DELIVERY_DEAD_LETTER);

    assert(environment_alarm_delivery_retry_delay_ms(0U) == 1000U);
    assert(environment_alarm_delivery_retry_delay_ms(1U) == 2000U);
    assert(environment_alarm_delivery_retry_delay_ms(5U) == 32000U);
    assert(environment_alarm_delivery_retry_delay_ms(6U) == 60000U);
    assert(environment_alarm_delivery_retry_delay_ms(UINT8_MAX) == 60000U);

    puts("environment_alarm_delivery_host: PASS");
    return 0;
}
