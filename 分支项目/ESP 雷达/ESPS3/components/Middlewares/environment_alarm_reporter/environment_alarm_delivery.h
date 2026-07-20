#ifndef ENVIRONMENT_ALARM_DELIVERY_H
#define ENVIRONMENT_ALARM_DELIVERY_H

#include <stdint.h>

#include "esp_err.h"

typedef enum {
    ENVIRONMENT_ALARM_DELIVERY_SENT = 0,
    ENVIRONMENT_ALARM_DELIVERY_RETRY,
    ENVIRONMENT_ALARM_DELIVERY_DEAD_LETTER,
} environment_alarm_delivery_outcome_t;

environment_alarm_delivery_outcome_t environment_alarm_delivery_classify(esp_err_t result,
                                                                           int http_status);
uint32_t environment_alarm_delivery_retry_delay_ms(uint8_t retry_count);

#endif /* ENVIRONMENT_ALARM_DELIVERY_H */
