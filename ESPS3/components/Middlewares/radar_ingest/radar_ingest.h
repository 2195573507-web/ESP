#ifndef RADAR_INGEST_H
#define RADAR_INGEST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef RADAR_INGEST_HOST_TEST
#include "esp_err.h"
#else
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_NO_MEM (-1)
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define RADAR_INGEST_MAX_BODY_BYTES 1024U
#define RADAR_INGEST_HISTORY_DEPTH 64U

typedef enum {
    RADAR_INGEST_ACCEPTED = 0,
    RADAR_INGEST_INVALID_ARGUMENT,
    RADAR_INGEST_TOO_LARGE,
    RADAR_INGEST_INVALID_JSON,
    RADAR_INGEST_INVALID_SCHEMA,
    RADAR_INGEST_INVALID_LOCAL_ID,
    RADAR_INGEST_IDENTITY_MISMATCH,
    RADAR_INGEST_INVALID_TARGETS,
    RADAR_INGEST_UNAVAILABLE,
} radar_ingest_result_t;

typedef struct {
    uint16_t count;
    uint16_t capacity;
    bool psram_backed;
} radar_ingest_history_stats_t;

/* Starts the latest-frame worker after the stable radar domain is initialized. */
esp_err_t radar_ingest_start(void);

/* HTTP path: validate C5 v3 payload and atomically replace that source's pending frame. */
radar_ingest_result_t radar_ingest_process_json(const char *json,
                                                size_t json_len,
                                                uint64_t received_at_ms);

/* Exposed for host tests; runtime calls this only from radar_worker_task. */
void radar_ingest_process_pending(uint64_t now_ms);
bool radar_ingest_history_get_stats(radar_ingest_history_stats_t *out);
const char *radar_ingest_result_name(radar_ingest_result_t result);

#ifdef __cplusplus
}
#endif

#endif
