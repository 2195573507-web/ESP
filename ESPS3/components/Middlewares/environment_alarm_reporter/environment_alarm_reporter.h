#ifndef ENVIRONMENT_ALARM_REPORTER_H
#define ENVIRONMENT_ALARM_REPORTER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ENVIRONMENT_ALARM_REPORT_QUEUE_CAPACITY 24U

typedef struct {
    uint64_t events_enqueued;
    uint64_t events_sent;
    uint64_t retries;
    uint64_t queue_full;
    uint64_t dead_letter;
    uint64_t engine_ack_failures;
    size_t engine_queue_depth;
    size_t report_queue_depth;
    bool ready;
} environment_alarm_reporter_stats_t;

/** @brief Initializes the fixed event FIFO and its low-priority consumer task. */
esp_err_t environment_alarm_reporter_init(void);

/**
 * @brief Transfers engine events to the independent pending FIFO, then acks
 * only the accepted contiguous engine prefix.
 */
esp_err_t environment_alarm_reporter_drain_engine(void);

/** @brief Copies current delivery and engine queue diagnostics. */
esp_err_t environment_alarm_reporter_get_stats(environment_alarm_reporter_stats_t *out);

#ifdef __cplusplus
}
#endif

#endif /* ENVIRONMENT_ALARM_REPORTER_H */
