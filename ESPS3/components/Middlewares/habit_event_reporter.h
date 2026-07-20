#ifndef HABIT_EVENT_REPORTER_H
#define HABIT_EVENT_REPORTER_H

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t sent_events;
    uint32_t retry_attempts;
    uint32_t dropped_events;
    uint32_t pending_events;
} habit_event_reporter_stats_t;

/* Runs independently from radar and habit evaluation tasks. */
esp_err_t habit_event_reporter_start(void);
void habit_event_reporter_get_stats(habit_event_reporter_stats_t *out);

#ifdef __cplusplus
}
#endif

#endif
