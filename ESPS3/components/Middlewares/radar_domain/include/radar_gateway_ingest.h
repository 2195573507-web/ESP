#ifndef RADAR_GATEWAY_INGEST_H
#define RADAR_GATEWAY_INGEST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef RADAR_GATEWAY_HOST_TEST
#include "esp_err.h"
#else
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_NO_MEM (-1)
#endif
#include "radar_registry.h"
#include "radar_spatial_state.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RADAR_GATEWAY_MAX_REMOTE_SOURCES 2U
#define RADAR_GATEWAY_DEVICE_ID_LEN RADAR_REGISTRY_DEVICE_ID_LEN
#define RADAR_GATEWAY_OFFLINE_TIMEOUT_MS 5000U
#define RADAR_GATEWAY_MAX_BODY_BYTES 1024U

typedef struct {
    uint8_t slot;
    int16_t x_mm;
    int16_t y_mm;
    int16_t speed_cm_s;
    uint16_t resolution_mm;
    uint32_t distance_mm;
    uint8_t confidence;
    bool valid;
} radar_gateway_target_t;

typedef struct {
    uint8_t local_id;
    uint8_t link_state;
    bool sample_valid;
    uint32_t request_uptime_ms;
    uint32_t request_sequence;
    uint32_t frame_seq;
    uint32_t frame_uptime_ms;
    uint8_t target_count;
    radar_gateway_target_t targets[LD2450_MAX_TARGETS];
} radar_gateway_sample_t;

typedef struct {
    uint32_t track_id;
    int32_t x_mm;
    int32_t y_mm;
    int16_t speed_cm_s;
    uint16_t resolution_mm;
    uint32_t distance_mm;
    uint8_t confidence;
    uint8_t zone_id;
    bool visible;
} radar_gateway_target_output_t;

typedef struct {
    uint8_t zone_id;
    radar_zone_type_t type;
    uint8_t target_count;
} radar_gateway_zone_output_t;

typedef struct {
    char device_id[RADAR_GATEWAY_DEVICE_ID_LEN];
    uint8_t local_id;
    bool radar_online;
    radar_occupancy_state_t occupancy;
    radar_motion_state_t motion;
    uint64_t updated_at_ms;
    uint8_t target_count;
    radar_gateway_target_output_t targets[LD2450_MAX_TARGETS];
    uint8_t zone_count;
    radar_gateway_zone_output_t zones[RADAR_ZONE_MAP_MAX_ZONES];
} radar_gateway_output_t;

typedef enum {
    RADAR_GATEWAY_INGEST_ACCEPTED = 0,
    RADAR_GATEWAY_INGEST_DUPLICATE,
    RADAR_GATEWAY_INGEST_INVALID_ARGUMENT,
    RADAR_GATEWAY_INGEST_TOO_LARGE,
    RADAR_GATEWAY_INGEST_INVALID_JSON,
    RADAR_GATEWAY_INGEST_INVALID_SCHEMA,
    RADAR_GATEWAY_INGEST_INVALID_LOCAL_ID,
    RADAR_GATEWAY_INGEST_INVALID_SEQUENCE,
    RADAR_GATEWAY_INGEST_INVALID_TARGETS,
    RADAR_GATEWAY_INGEST_IDENTITY_MISMATCH,
    RADAR_GATEWAY_INGEST_SEQUENCE_CONFLICT,
    RADAR_GATEWAY_INGEST_SEQUENCE_BACKWARD,
    RADAR_GATEWAY_INGEST_UNAVAILABLE,
} radar_gateway_ingest_result_t;

esp_err_t radar_gateway_ingest_start(void);

radar_gateway_ingest_result_t radar_gateway_ingest_admit(
    const radar_gateway_sample_t *sample,
    uint32_t session_generation,
    uint64_t received_at_ms,
    radar_gateway_output_t *out);

radar_gateway_ingest_result_t radar_gateway_ingest_process_json(
    const char *json,
    size_t json_len,
    const char *device_id,
    uint64_t received_at_ms,
    radar_gateway_sample_t *out_sample,
    radar_gateway_output_t *out);

bool radar_gateway_ingest_get_output(uint8_t local_id, radar_gateway_output_t *out);
void radar_gateway_ingest_poll(uint64_t now_ms);
const char *radar_gateway_ingest_result_name(radar_gateway_ingest_result_t result);
const char *radar_gateway_occupancy_name(radar_occupancy_state_t state);
const char *radar_gateway_motion_name(radar_motion_state_t state);
const char *radar_gateway_tracking_name(const radar_gateway_output_t *output);

#ifdef __cplusplus
}
#endif

#endif
