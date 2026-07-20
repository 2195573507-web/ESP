#ifndef ENVIRONMENT_ALARM_ADAPTER_H
#define ENVIRONMENT_ALARM_ADAPTER_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "protocol_adapter.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint64_t samples_received;
    uint64_t samples_valid;
    uint64_t samples_invalid;
    uint64_t duplicates;
    uint64_t out_of_order;
    uint64_t sequence_wraps;
    uint64_t restarts;
    uint64_t missing_field;
    bool ready;
} environment_alarm_adapter_stats_t;

/** @brief Initializes real-BME normalization and the environment reporter. */
esp_err_t environment_alarm_adapter_init(void);

/** @brief Handles one already parsed and validated BME690 local envelope. */
esp_err_t environment_alarm_adapter_ingest(const protocol_adapter_envelope_t *envelope);

/** @brief Returns aggregate per-device ingress diagnostics. */
esp_err_t environment_alarm_adapter_get_stats(environment_alarm_adapter_stats_t *out);

#ifdef __cplusplus
}
#endif

#endif /* ENVIRONMENT_ALARM_ADAPTER_H */
