#include <assert.h>
#include <stdio.h>

#include "environment_alarm_sequence.h"

int main(void)
{
    assert(environment_alarm_sequence_classify(100U, 100U) ==
           ENVIRONMENT_ALARM_SEQUENCE_DUPLICATE);
    assert(environment_alarm_sequence_classify(101U, 100U) ==
           ENVIRONMENT_ALARM_SEQUENCE_NEW);
    assert(environment_alarm_sequence_classify(99U, 100U) ==
           ENVIRONMENT_ALARM_SEQUENCE_OUT_OF_ORDER);
    assert(environment_alarm_sequence_classify(1U, UINT32_MAX - 1U) ==
           ENVIRONMENT_ALARM_SEQUENCE_WRAP);
    puts("environment_alarm_adapter_host: PASS");
    return 0;
}
