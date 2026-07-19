#ifndef RADAR_INGEST_H
#define RADAR_INGEST_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RADAR_INGEST_MAX_BODY_BYTES 1024U

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

radar_ingest_result_t radar_ingest_process_json(const char *json,
                                                size_t json_len,
                                                uint64_t received_at_ms);

const char *radar_ingest_result_name(radar_ingest_result_t result);

#ifdef __cplusplus
}
#endif

#endif
