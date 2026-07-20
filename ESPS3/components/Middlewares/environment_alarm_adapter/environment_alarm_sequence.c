#include "environment_alarm_sequence.h"

environment_alarm_sequence_result_t environment_alarm_sequence_classify(uint32_t current,
                                                                         uint32_t previous)
{
    if (current == previous) {
        return ENVIRONMENT_ALARM_SEQUENCE_DUPLICATE;
    }
    const uint32_t delta = current - previous;
    if (delta < UINT32_C(0x80000000)) {
        return current < previous ? ENVIRONMENT_ALARM_SEQUENCE_WRAP :
                                    ENVIRONMENT_ALARM_SEQUENCE_NEW;
    }
    return ENVIRONMENT_ALARM_SEQUENCE_OUT_OF_ORDER;
}
