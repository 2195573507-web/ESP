#ifndef ESP_TIMER_H
#define ESP_TIMER_H

#include <stdint.h>

#ifdef ENV_ALARM_HOST_MUTABLE_TIME
int64_t environment_alarm_test_time_us(void);
#else
static inline int64_t environment_alarm_test_time_us(void)
{
    return 0;
}
#endif

static inline int64_t esp_timer_get_time(void)
{
    return environment_alarm_test_time_us();
}

#endif /* ESP_TIMER_H */
