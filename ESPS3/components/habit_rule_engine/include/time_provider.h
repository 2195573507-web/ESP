#ifndef HABIT_TIME_PROVIDER_H
#define HABIT_TIME_PROVIDER_H

#include "habit_rule_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

bool habit_time_provider_unavailable(void *context, habit_wall_clock_t *out);

#ifdef __cplusplus
}
#endif

#endif
