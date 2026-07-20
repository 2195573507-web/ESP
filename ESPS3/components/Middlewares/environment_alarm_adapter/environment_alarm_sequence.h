#ifndef ENVIRONMENT_ALARM_SEQUENCE_H
#define ENVIRONMENT_ALARM_SEQUENCE_H

#include <stdint.h>

typedef enum {
    ENVIRONMENT_ALARM_SEQUENCE_NEW = 0,
    ENVIRONMENT_ALARM_SEQUENCE_DUPLICATE,
    ENVIRONMENT_ALARM_SEQUENCE_OUT_OF_ORDER,
    ENVIRONMENT_ALARM_SEQUENCE_WRAP,
} environment_alarm_sequence_result_t;

/** @brief Classifies uint32_t remote sequence values using modular ordering. */
environment_alarm_sequence_result_t environment_alarm_sequence_classify(uint32_t current,
                                                                         uint32_t previous);

#endif /* ENVIRONMENT_ALARM_SEQUENCE_H */
