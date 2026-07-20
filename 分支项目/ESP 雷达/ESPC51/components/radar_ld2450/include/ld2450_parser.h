#ifndef LD2450_PARSER_H
#define LD2450_PARSER_H

#include <stddef.h>
#include <stdint.h>

#include "ld2450_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t bytes_received;
    uint32_t candidate_frames;
    uint32_t valid_frames;
    uint32_t bad_header;
    uint32_t bad_length;
    uint32_t bad_tail;
    uint32_t skipped_bytes;
    uint32_t invalid_target_slots;
    uint32_t coordinate_outliers;
    /* Deprecated compatibility aliases. New consumers use the fields above. */
    uint32_t invalid_tail_frames;
    uint32_t discarded_bytes;
    uint32_t partial_timeouts;
    uint32_t partial_timeout_keep_count;
    uint32_t partial_force_reset_count;
    uint32_t resync_count;
    uint32_t frame_rate_millihz;
    uint32_t valid_frame_rate_millihz;
    uint32_t max_frame_interval_ms;
    uint64_t last_valid_frame_ms;
    uint64_t last_rx_ms;
    uint64_t partial_last_change_ms;
    uint32_t partial_length;
} ld2450_parser_diagnostics_t;

typedef struct {
    uint8_t buffer[LD2450_FRAME_SIZE];
    size_t length;
    uint32_t next_frame_seq;
    uint64_t rate_window_start_ms;
    uint32_t rate_window_candidates;
    uint32_t rate_window_valid;
    uint64_t last_rx_ms;
    uint64_t partial_last_change_ms;
    bool resync_active;
    ld2450_parser_diagnostics_t diagnostics;
} ld2450_parser_t;

typedef void (*ld2450_frame_callback_t)(const radar_frame_t *frame, void *ctx);

void ld2450_parser_init(ld2450_parser_t *parser);
void ld2450_parser_reset(ld2450_parser_t *parser);
size_t ld2450_parser_feed(ld2450_parser_t *parser,
                          const uint8_t *data,
                          size_t data_len,
                          uint64_t received_at_ms,
                          ld2450_frame_callback_t callback,
                          void *ctx);
void ld2450_parser_note_timeout(ld2450_parser_t *parser, uint64_t now_ms);
void ld2450_parser_force_reset(ld2450_parser_t *parser);
void ld2450_parser_get_diagnostics(const ld2450_parser_t *parser,
                                   ld2450_parser_diagnostics_t *out);
int16_t ld2450_decode_directional(uint8_t lo, uint8_t hi);
int16_t ld2450_decode_y(uint8_t lo, uint8_t hi);
uint32_t ld2450_target_distance_mm(const radar_target_t *target);

#ifdef __cplusplus
}
#endif

#endif
