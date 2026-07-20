#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ld2450_parser.h"
#include "radar_presence.h"
#include "radar_state_codec.h"

typedef struct {
    radar_frame_t frames[8];
    size_t count;
} capture_t;

static void capture_frame(const radar_frame_t *frame, void *ctx)
{
    capture_t *capture = ctx;
    assert(capture != NULL);
    assert(capture->count < sizeof(capture->frames) / sizeof(capture->frames[0]));
    capture->frames[capture->count++] = *frame;
}

static void put_u16(uint8_t *out, uint16_t value)
{
    out[0] = (uint8_t)(value & 0xffU);
    out[1] = (uint8_t)(value >> 8);
}

static uint16_t encode_directional(int16_t value)
{
    return value >= 0 ? (uint16_t)(32768 + value) : (uint16_t)(-value);
}

static void put_target(uint8_t *slot,
                       int16_t x,
                       int16_t y,
                       int16_t speed,
                       uint16_t resolution)
{
    put_u16(&slot[0], encode_directional(x));
    put_u16(&slot[2], (uint16_t)(32768 + y));
    put_u16(&slot[4], encode_directional(speed));
    put_u16(&slot[6], resolution);
}

static void make_frame(uint8_t frame[LD2450_FRAME_SIZE], uint8_t target_count)
{
    memset(frame, 0, LD2450_FRAME_SIZE);
    frame[0] = 0xAA;
    frame[1] = 0xFF;
    frame[2] = 0x03;
    frame[3] = 0x00;
    for (uint8_t i = 0; i < target_count && i < LD2450_MAX_TARGETS; ++i) {
        put_target(&frame[4U + i * 8U],
                   (int16_t)(100 + i * 10),
                   (int16_t)(1000 + i * 20),
                   (int16_t)(10 + i),
                   (uint16_t)(300 + i));
    }
    frame[28] = 0x55;
    frame[29] = 0xCC;
}

static radar_frame_t presence_frame(uint8_t target_count, uint64_t now_ms)
{
    radar_frame_t frame = {
        .frame_seq = (uint32_t)now_ms,
        .received_at_ms = now_ms,
        .target_count = target_count,
    };
    for (uint8_t i = 0; i < target_count; ++i) {
        frame.targets[i].valid = true;
        frame.targets[i].x_mm = (int16_t)(100 + i);
        frame.targets[i].y_mm = (int16_t)(1000 + i);
        frame.targets[i].speed_cm_s = 10;
        frame.targets[i].resolution_mm = 300;
    }
    return frame;
}

static void test_official_decode(void)
{
    uint8_t frame[LD2450_FRAME_SIZE];
    make_frame(frame, 0);
    const uint8_t sample[8] = {0x0E, 0x03, 0xB1, 0x86, 0x10, 0x00, 0x40, 0x01};
    memcpy(&frame[4], sample, sizeof(sample));

    ld2450_parser_t parser;
    capture_t capture = {0};
    ld2450_parser_init(&parser);
    assert(ld2450_parser_feed(&parser,
                              frame,
                              sizeof(frame),
                              123,
                              capture_frame,
                              &capture) == 1U);
    assert(capture.count == 1U);
    assert(capture.frames[0].target_count == 1U);
    assert(capture.frames[0].targets[0].x_mm == -782);
    assert(capture.frames[0].targets[0].y_mm == 1713);
    assert(capture.frames[0].targets[0].speed_cm_s == -16);
    assert(capture.frames[0].targets[0].resolution_mm == 320U);
    assert(capture.frames[0].targets[0].distance_mm == 1883U);
}

static void test_parser_streaming(void)
{
    uint8_t frame[LD2450_FRAME_SIZE];
    make_frame(frame, 3);

    for (size_t split = 0; split <= sizeof(frame); ++split) {
        ld2450_parser_t parser;
        capture_t capture = {0};
        ld2450_parser_init(&parser);
        (void)ld2450_parser_feed(&parser, frame, split, 10, capture_frame, &capture);
        (void)ld2450_parser_feed(&parser,
                                 frame + split,
                                 sizeof(frame) - split,
                                 11,
                                 capture_frame,
                                 &capture);
        assert(capture.count == 1U);
        assert(capture.frames[0].target_count == 3U);
    }

    uint8_t two_frames[LD2450_FRAME_SIZE * 2U];
    memcpy(two_frames, frame, sizeof(frame));
    memcpy(two_frames + sizeof(frame), frame, sizeof(frame));
    ld2450_parser_t parser;
    capture_t capture = {0};
    ld2450_parser_init(&parser);
    assert(ld2450_parser_feed(&parser,
                              two_frames,
                              sizeof(two_frames),
                              20,
                              capture_frame,
                              &capture) == 2U);
    assert(capture.count == 2U);

    /* A split frame remains buffered until its later bytes arrive. */
    ld2450_parser_init(&parser);
    capture.count = 0U;
    assert(ld2450_parser_feed(&parser, frame, 11U, 30U, capture_frame, &capture) == 0U);
    assert(parser.length == 11U);
    assert(ld2450_parser_feed(&parser,
                              frame + 11U,
                              sizeof(frame) - 11U,
                              31U,
                              capture_frame,
                              &capture) == 1U);
    assert(capture.count == 1U);
}

static void test_parser_resync_and_boundaries(void)
{
    uint8_t frame[LD2450_FRAME_SIZE];
    make_frame(frame, 1);
    frame[12] = 0xAA;
    frame[13] = 0xFF;
    frame[14] = 0x03;
    frame[15] = 0x00;

    uint8_t stream[7U + LD2450_FRAME_SIZE * 2U];
    memset(stream, 0x31, 7U);
    memcpy(stream + 7U, frame, sizeof(frame));
    stream[7U + LD2450_FRAME_SIZE - 1U] = 0x00;
    memcpy(stream + 7U + LD2450_FRAME_SIZE, frame, sizeof(frame));

    ld2450_parser_t parser;
    capture_t capture = {0};
    ld2450_parser_init(&parser);
    (void)ld2450_parser_feed(&parser,
                             stream,
                             sizeof(stream),
                             30,
                             capture_frame,
                             &capture);
    assert(capture.count == 1U);
    assert(parser.diagnostics.invalid_tail_frames >= 1U);
    assert(parser.diagnostics.discarded_bytes > 0U);
    assert(parser.diagnostics.bytes_received == sizeof(stream));
    assert(parser.diagnostics.bad_header > 0U);
    assert(parser.diagnostics.bad_tail >= 1U);
    assert(parser.diagnostics.skipped_bytes == parser.diagnostics.discarded_bytes);
    assert(parser.diagnostics.resync_count < parser.diagnostics.skipped_bytes);

    uint8_t half[15];
    memcpy(half, frame, sizeof(half));
    ld2450_parser_init(&parser);
    (void)ld2450_parser_feed(&parser, half, sizeof(half), 40, capture_frame, &capture);
    ld2450_parser_note_timeout(&parser, 100U);
    assert(parser.length == sizeof(half));
    assert(parser.diagnostics.partial_timeouts == 1U);
    assert(parser.diagnostics.partial_timeout_keep_count == 1U);
    ld2450_parser_note_timeout(&parser, 2040U);
    assert(parser.length == 0U);
    assert(parser.diagnostics.partial_force_reset_count == 1U);
    assert(parser.diagnostics.bad_length == 1U);

    uint8_t garbage[4096];
    memset(garbage, 0x5A, sizeof(garbage));
    ld2450_parser_init(&parser);
    (void)ld2450_parser_feed(&parser, garbage, sizeof(garbage), 50, NULL, NULL);
    assert(parser.length < LD2450_FRAME_SIZE);

    assert(ld2450_parser_feed(NULL, frame, sizeof(frame), 0, NULL, NULL) == 0U);
    assert(ld2450_parser_feed(&parser, NULL, 1U, 0, NULL, NULL) == 0U);
    assert(ld2450_parser_feed(&parser, NULL, 0U, 0, NULL, NULL) == 0U);
}

static void test_parser_timing_diagnostics(void)
{
    uint8_t frame[LD2450_FRAME_SIZE];
    make_frame(frame, 1U);
    ld2450_parser_t parser;
    capture_t capture = {0};
    ld2450_parser_init(&parser);
    assert(ld2450_parser_feed(&parser, frame, sizeof(frame), 100U, capture_frame, &capture) == 1U);
    assert(ld2450_parser_feed(&parser, frame, sizeof(frame), 1200U, capture_frame, &capture) == 1U);
    ld2450_parser_diagnostics_t diagnostics;
    ld2450_parser_get_diagnostics(&parser, &diagnostics);
    assert(diagnostics.last_valid_frame_ms == 1200U);
    assert(diagnostics.max_frame_interval_ms == 1100U);
    assert(diagnostics.frame_rate_millihz > 0U);
    assert(diagnostics.valid_frame_rate_millihz > 0U);
}

static void test_decode_extremes_and_zero_slots(void)
{
    assert(ld2450_decode_directional(0x01, 0x00) == -1);
    assert(ld2450_decode_directional(0x01, 0x80) == 1);
    assert(ld2450_decode_y(0x00, 0x80) == 0);

    uint8_t frame[LD2450_FRAME_SIZE];
    make_frame(frame, 1);
    put_u16(&frame[10], UINT16_MAX);

    ld2450_parser_t parser;
    capture_t capture = {0};
    ld2450_parser_init(&parser);
    (void)ld2450_parser_feed(&parser, frame, sizeof(frame), 60, capture_frame, &capture);
    assert(capture.count == 1U);
    assert(capture.frames[0].target_count == 1U);
    assert(!capture.frames[0].targets[1].valid);
    assert(!capture.frames[0].targets[2].valid);
    assert(capture.frames[0].targets[0].resolution_mm == UINT16_MAX);

    make_frame(frame, 1U);
    put_target(&frame[4], 100, -32704, 0, 100U);
    ld2450_parser_init(&parser);
    capture.count = 0U;
    assert(ld2450_parser_feed(&parser, frame, sizeof(frame), 70U, capture_frame, &capture) == 1U);
    assert(capture.count == 1U);
    assert(capture.frames[0].target_count == 0U);
    assert(parser.diagnostics.invalid_target_slots == 3U);
}

static void test_presence_state_machine(void)
{
    radar_presence_t presence;
    radar_snapshot_t snapshot;
    radar_presence_init(&presence, NULL, 0);
    radar_presence_get_snapshot(&presence, &snapshot);
    assert(snapshot.state == RADAR_STATE_UNKNOWN);

    radar_frame_t target = presence_frame(1, 100);
    radar_presence_on_frame(&presence, &target, true, 100);
    radar_presence_get_snapshot(&presence, &snapshot);
    assert(snapshot.state == RADAR_STATE_UNKNOWN);

    target = presence_frame(1, 200);
    radar_presence_on_frame(&presence, &target, true, 200);
    radar_presence_get_snapshot(&presence, &snapshot);
    assert(snapshot.state == RADAR_STATE_MOTION);

    radar_frame_t empty = presence_frame(0, 1199);
    radar_presence_on_frame(&presence, &empty, true, 1199);
    radar_presence_get_snapshot(&presence, &snapshot);
    assert(snapshot.state == RADAR_STATE_MOTION);

    empty = presence_frame(0, 1200);
    radar_presence_on_frame(&presence, &empty, true, 1200);
    radar_presence_get_snapshot(&presence, &snapshot);
    assert(snapshot.state == RADAR_STATE_HOLD);

    target = presence_frame(1, 1300);
    radar_presence_on_frame(&presence, &target, true, 1300);
    radar_presence_get_snapshot(&presence, &snapshot);
    assert(snapshot.state == RADAR_STATE_MOTION);

    empty = presence_frame(0, 2300);
    radar_presence_on_frame(&presence, &empty, true, 2300);
    empty = presence_frame(0, 2301);
    radar_presence_on_frame(&presence, &empty, true, 2301);
    radar_presence_get_snapshot(&presence, &snapshot);
    assert(snapshot.state == RADAR_STATE_HOLD);

    empty = presence_frame(0, 901300);
    radar_presence_on_frame(&presence, &empty, true, 901300);
    radar_presence_get_snapshot(&presence, &snapshot);
    assert(snapshot.state == RADAR_STATE_VACANT_INFERRED);

    radar_presence_poll(&presence, true, 904301);
    radar_presence_get_snapshot(&presence, &snapshot);
    assert(snapshot.state == RADAR_STATE_UNKNOWN);
    assert(!snapshot.frame_fresh);

    target = presence_frame(1, 904400);
    radar_presence_on_frame(&presence, &target, true, 904400);
    radar_presence_get_snapshot(&presence, &snapshot);
    assert(snapshot.state == RADAR_STATE_UNKNOWN);
    target = presence_frame(1, 904500);
    radar_presence_on_frame(&presence, &target, true, 904500);
    radar_presence_get_snapshot(&presence, &snapshot);
    assert(snapshot.state == RADAR_STATE_MOTION);

    radar_presence_note_uart_error(&presence, 904600);
    radar_presence_get_snapshot(&presence, &snapshot);
    assert(snapshot.state == RADAR_STATE_UNKNOWN);
    assert(!snapshot.uart_online);
}

static void test_presence_offline_profiles_and_time_rollback(void)
{
    radar_presence_config_t fast = radar_presence_default_config();
    fast.hold_timeout_ms = 2000U;
    radar_presence_t room_a;
    radar_presence_t room_b;
    radar_presence_init(&room_a, &fast, 0);
    radar_presence_init(&room_b, NULL, 0);

    radar_frame_t target = presence_frame(1, 10);
    radar_presence_on_frame(&room_a, &target, true, 10);
    radar_presence_on_frame(&room_b, &target, true, 10);
    target = presence_frame(1, 20);
    radar_presence_on_frame(&room_a, &target, true, 20);
    radar_presence_on_frame(&room_b, &target, true, 20);

    radar_frame_t empty = presence_frame(0, 1020);
    radar_presence_on_frame(&room_a, &empty, true, 1020);
    radar_presence_on_frame(&room_b, &empty, true, 1020);
    empty = presence_frame(0, 1021);
    radar_presence_on_frame(&room_a, &empty, true, 1021);
    radar_presence_on_frame(&room_b, &empty, true, 1021);
    empty = presence_frame(0, 2020);
    radar_presence_on_frame(&room_a, &empty, false, 2020);

    radar_snapshot_t snapshot_a;
    radar_snapshot_t snapshot_b;
    radar_presence_get_snapshot(&room_a, &snapshot_a);
    radar_presence_get_snapshot(&room_b, &snapshot_b);
    assert(snapshot_a.state == RADAR_STATE_UNKNOWN);
    assert(snapshot_b.state == RADAR_STATE_HOLD);

    radar_presence_poll(&room_b, true, 1000);
    radar_presence_get_snapshot(&room_b, &snapshot_b);
    assert(snapshot_b.state == RADAR_STATE_UNKNOWN);

    radar_presence_diagnostics_t diagnostics;
    radar_presence_get_diagnostics(&room_b, &diagnostics);
    assert(diagnostics.time_rollback_count == 1U);
}

static void test_state_codec(void)
{
    radar_snapshot_t snapshot = {
        .state = RADAR_STATE_MOTION,
        .current_target_count = 3,
        .state_seq = 7,
        .state_since_ms = 100,
        .last_valid_frame_ms = 900,
        .last_motion_ms = 950,
        .uart_online = true,
        .frame_fresh = true,
    };
    for (size_t i = 0; i < LD2450_MAX_TARGETS; ++i) {
        snapshot.targets[i].valid = true;
        snapshot.targets[i].x_mm = i == 0U ? INT16_MIN : (int16_t)(100 + i);
        snapshot.targets[i].y_mm = i == 2U ? INT16_MAX : (int16_t)(1000 + i);
        snapshot.targets[i].speed_cm_s = (int16_t)(-20 + (int)i);
        snapshot.targets[i].resolution_mm = i == 1U ? UINT16_MAX : 320U;
    }

    char json[RADAR_STATE_JSON_MAX_BYTES];
    int len = radar_state_encode_json(&snapshot, 1U, UINT32_MAX, 1000U, json, sizeof(json));
    assert(len > 0);
    assert((size_t)len < sizeof(json));
    assert(strstr(json, "\"target_count\":3") != NULL);
    assert(strstr(json, "\"state\":\"motion\"") != NULL);
    assert(strstr(json, "raw") == NULL);
    assert(strstr(json, "phase") == NULL);
    assert(strstr(json, "subcarrier") == NULL);
    assert(strstr(json, "csi") == NULL);

    snapshot.current_target_count = 2U;
    assert(radar_state_encode_json(&snapshot, 1U, 1U, 1000U, json, sizeof(json)) < 0);
    assert(radar_state_encode_json(&snapshot, 0U, 1U, 1000U, json, sizeof(json)) < 0);
    assert(radar_state_encode_json(&snapshot, 1U, 1U, 1000U, json, 32U) < 0);
}


int main(void)
{
    test_official_decode();
    test_parser_streaming();
    test_parser_resync_and_boundaries();
    test_parser_timing_diagnostics();
    test_decode_extremes_and_zero_slots();
    test_presence_state_machine();
    test_presence_offline_profiles_and_time_rollback();
    test_state_codec();
    puts("radar core host tests: PASS");
    return 0;
}
