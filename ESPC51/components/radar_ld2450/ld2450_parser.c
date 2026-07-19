#include "ld2450_parser.h"

#include <limits.h>
#include <string.h>

/*
 * 字节流解析器只接受完整的固定长度 LD2450 帧。遇到错误时逐字节重新同步，
 * 因而损坏数据不会让后续有效帧永久丢失；诊断计数用于区分丢帧与协议错误。
 */

static const uint8_t s_header[LD2450_HEADER_SIZE] = {0xAA, 0xFF, 0x03, 0x00};
#define LD2450_PARTIAL_FORCE_RESET_MS 2000U

static void sat_inc_u32(uint32_t *value)
{
    if (value != NULL && *value < UINT32_MAX) {
        ++(*value);
    }
}

static void sat_add_u32(uint32_t *value, size_t amount)
{
    if (value == NULL || *value == UINT32_MAX) {
        return;
    }
    const uint64_t total = (uint64_t)*value + amount;
    *value = total > UINT32_MAX ? UINT32_MAX : (uint32_t)total;
}

static uint32_t rate_millihz(uint32_t count, uint64_t elapsed_ms)
{
    if (elapsed_ms == 0U) {
        return 0U;
    }
    const uint64_t rate = ((uint64_t)count * 1000000ULL) / elapsed_ms;
    return rate > UINT32_MAX ? UINT32_MAX : (uint32_t)rate;
}

static void note_candidate(ld2450_parser_t *parser,
                           uint64_t received_at_ms,
                           bool valid)
{
    if (parser->rate_window_start_ms == 0U) {
        parser->rate_window_start_ms = received_at_ms;
    }
    sat_inc_u32(&parser->diagnostics.candidate_frames);
    sat_inc_u32(&parser->rate_window_candidates);
    if (valid) {
        sat_inc_u32(&parser->rate_window_valid);
    }
    if (received_at_ms >= parser->rate_window_start_ms &&
        received_at_ms - parser->rate_window_start_ms >= 1000U) {
        const uint64_t elapsed_ms = received_at_ms - parser->rate_window_start_ms;
        parser->diagnostics.frame_rate_millihz =
            rate_millihz(parser->rate_window_candidates, elapsed_ms);
        parser->diagnostics.valid_frame_rate_millihz =
            rate_millihz(parser->rate_window_valid, elapsed_ms);
        parser->rate_window_start_ms = received_at_ms;
        parser->rate_window_candidates = 0U;
        parser->rate_window_valid = 0U;
    }
}

static void note_valid_interval(ld2450_parser_t *parser, uint64_t received_at_ms)
{
    if (parser->diagnostics.last_valid_frame_ms > 0U &&
        received_at_ms >= parser->diagnostics.last_valid_frame_ms) {
        const uint64_t interval = received_at_ms - parser->diagnostics.last_valid_frame_ms;
        if (interval > parser->diagnostics.max_frame_interval_ms) {
            parser->diagnostics.max_frame_interval_ms =
                interval > UINT32_MAX ? UINT32_MAX : (uint32_t)interval;
        }
    }
    parser->diagnostics.last_valid_frame_ms = received_at_ms;
}

int16_t ld2450_decode_directional(uint8_t lo, uint8_t hi)
{
    uint16_t raw = (uint16_t)lo | ((uint16_t)hi << 8);
    int32_t value = (hi & 0x80U) != 0U ? (int32_t)raw - 32768 : -(int32_t)raw;
    return (int16_t)value;
}

int16_t ld2450_decode_y(uint8_t lo, uint8_t hi)
{
    uint16_t raw = (uint16_t)lo | ((uint16_t)hi << 8);
    return (int16_t)((int32_t)raw - 32768);
}

static uint32_t integer_sqrt_u64(uint64_t value)
{
    uint64_t bit = 1ULL << 62;
    uint64_t root = 0U;

    while (bit > value) {
        bit >>= 2;
    }
    while (bit != 0U) {
        if (value >= root + bit) {
            value -= root + bit;
            root = (root >> 1) + bit;
        } else {
            root >>= 1;
        }
        bit >>= 2;
    }
    return (uint32_t)root;
}

uint32_t ld2450_target_distance_mm(const radar_target_t *target)
{
    if (target == NULL || !target->valid) {
        return 0U;
    }

    const int64_t x = target->x_mm;
    const int64_t y = target->y_mm;
    return integer_sqrt_u64((uint64_t)(x * x) + (uint64_t)(y * y));
}

static bool target_slot_is_zero(const uint8_t *slot)
{
    for (size_t i = 0; i < LD2450_TARGET_SIZE; ++i) {
        if (slot[i] != 0U) {
            return false;
        }
    }
    return true;
}

static void parse_frame(ld2450_parser_t *parser,
                        uint64_t received_at_ms,
                        radar_frame_t *out)
{
    memset(out, 0, sizeof(*out));
    sat_inc_u32(&parser->next_frame_seq);
    out->frame_seq = parser->next_frame_seq;
    out->received_at_ms = received_at_ms;

    for (size_t i = 0; i < LD2450_MAX_TARGETS; ++i) {
        const uint8_t *slot = &parser->buffer[LD2450_HEADER_SIZE + i * LD2450_TARGET_SIZE];
        radar_target_t *target = &out->targets[i];
        if (target_slot_is_zero(slot)) {
            sat_inc_u32(&parser->diagnostics.invalid_target_slots);
            continue;
        }

        target->valid = true;
        target->slot = (uint8_t)i;
        target->x_mm = ld2450_decode_directional(slot[0], slot[1]);
        target->y_mm = ld2450_decode_y(slot[2], slot[3]);
        target->speed_cm_s = ld2450_decode_directional(slot[4], slot[5]);
        target->resolution_mm = (uint16_t)slot[6] | ((uint16_t)slot[7] << 8);
        target->distance_mm = ld2450_target_distance_mm(target);
        if (target->x_mm == INT16_MIN || target->x_mm == -32704 ||
            target->y_mm == INT16_MIN || target->y_mm == -32704) {
            memset(target, 0, sizeof(*target));
            sat_inc_u32(&parser->diagnostics.invalid_target_slots);
            continue;
        }
        ++out->target_count;
    }
}

void ld2450_parser_init(ld2450_parser_t *parser)
{
    if (parser != NULL) {
        memset(parser, 0, sizeof(*parser));
    }
}

void ld2450_parser_reset(ld2450_parser_t *parser)
{
    if (parser == NULL) {
        return;
    }
    parser->length = 0U;
    memset(parser->buffer, 0, sizeof(parser->buffer));
    parser->partial_last_change_ms = 0U;
    parser->resync_active = false;
}

void ld2450_parser_force_reset(ld2450_parser_t *parser)
{
    if (parser == NULL) {
        return;
    }
    if (parser->length > 0U) {
        sat_inc_u32(&parser->diagnostics.partial_force_reset_count);
        sat_inc_u32(&parser->diagnostics.bad_length);
        sat_add_u32(&parser->diagnostics.skipped_bytes, parser->length);
        sat_add_u32(&parser->diagnostics.discarded_bytes, parser->length);
        if (!parser->resync_active) {
            sat_inc_u32(&parser->diagnostics.resync_count);
            parser->resync_active = true;
        }
    }
    ld2450_parser_reset(parser);
}

static void discard_first_byte(ld2450_parser_t *parser, bool bad_header)
{
    if (parser->length == 0U) {
        return;
    }
    --parser->length;
    if (parser->length > 0U) {
        memmove(parser->buffer, parser->buffer + 1, parser->length);
    }
    sat_inc_u32(&parser->diagnostics.skipped_bytes);
    sat_inc_u32(&parser->diagnostics.discarded_bytes);
    if (bad_header) {
        sat_inc_u32(&parser->diagnostics.bad_header);
    }
    if (!parser->resync_active) {
        sat_inc_u32(&parser->diagnostics.resync_count);
        parser->resync_active = true;
    }
}

static bool buffer_is_header_prefix(const ld2450_parser_t *parser)
{
    return parser->length <= LD2450_HEADER_SIZE &&
           memcmp(parser->buffer, s_header, parser->length) == 0;
}

static bool consume_byte(ld2450_parser_t *parser,
                         uint8_t byte,
                         uint64_t received_at_ms,
                         ld2450_frame_callback_t callback,
                         void *ctx)
{
    if (parser->length >= sizeof(parser->buffer)) {
        sat_inc_u32(&parser->diagnostics.bad_length);
        discard_first_byte(parser, false);
    }
    parser->buffer[parser->length++] = byte;

    while (parser->length > 0U) {
        if (parser->length < LD2450_HEADER_SIZE) {
            if (buffer_is_header_prefix(parser)) {
                return false;
            }
            discard_first_byte(parser, true);
            continue;
        }

        if (memcmp(parser->buffer, s_header, sizeof(s_header)) != 0) {
            discard_first_byte(parser, true);
            continue;
        }
        if (parser->length < LD2450_FRAME_SIZE) {
            return false;
        }

        if (parser->buffer[LD2450_FRAME_SIZE - 2U] == 0x55U &&
            parser->buffer[LD2450_FRAME_SIZE - 1U] == 0xCCU) {
            radar_frame_t frame;
            parse_frame(parser, received_at_ms, &frame);
            sat_inc_u32(&parser->diagnostics.valid_frames);
            note_candidate(parser, received_at_ms, true);
            note_valid_interval(parser, received_at_ms);
            parser->length = 0U;
            parser->resync_active = false;
            if (callback != NULL) {
                callback(&frame, ctx);
            }
            return true;
        }

        sat_inc_u32(&parser->diagnostics.bad_tail);
        sat_inc_u32(&parser->diagnostics.invalid_tail_frames);
        note_candidate(parser, received_at_ms, false);
        discard_first_byte(parser, false);
    }
    return false;
}

size_t ld2450_parser_feed(ld2450_parser_t *parser,
                          const uint8_t *data,
                          size_t data_len,
                          uint64_t received_at_ms,
                          ld2450_frame_callback_t callback,
                          void *ctx)
{
    if (parser == NULL || (data == NULL && data_len > 0U)) {
        return 0U;
    }

    sat_add_u32(&parser->diagnostics.bytes_received, data_len);
    size_t published = 0U;
    for (size_t i = 0; i < data_len; ++i) {
        parser->last_rx_ms = received_at_ms;
        parser->partial_last_change_ms = received_at_ms;
        if (consume_byte(parser, data[i], received_at_ms, callback, ctx)) {
            ++published;
        }
    }
    parser->diagnostics.last_rx_ms = parser->last_rx_ms;
    parser->diagnostics.partial_last_change_ms = parser->partial_last_change_ms;
    parser->diagnostics.partial_length = (uint32_t)parser->length;
    return published;
}

void ld2450_parser_note_timeout(ld2450_parser_t *parser, uint64_t now_ms)
{
    if (parser == NULL) {
        return;
    }
    if (parser->length == 0U) {
        return;
    }
    sat_inc_u32(&parser->diagnostics.partial_timeouts);

    const bool aged = now_ms >= parser->partial_last_change_ms &&
                      now_ms - parser->partial_last_change_ms >= LD2450_PARTIAL_FORCE_RESET_MS;
    if (aged) {
        ld2450_parser_force_reset(parser);
    } else {
        sat_inc_u32(&parser->diagnostics.partial_timeout_keep_count);
    }
    parser->diagnostics.last_rx_ms = parser->last_rx_ms;
    parser->diagnostics.partial_last_change_ms = parser->partial_last_change_ms;
    parser->diagnostics.partial_length = (uint32_t)parser->length;
}

void ld2450_parser_get_diagnostics(const ld2450_parser_t *parser,
                                   ld2450_parser_diagnostics_t *out)
{
    if (parser != NULL && out != NULL) {
        *out = parser->diagnostics;
        out->last_rx_ms = parser->last_rx_ms;
        out->partial_last_change_ms = parser->partial_last_change_ms;
        out->partial_length = (uint32_t)parser->length;
    }
}
