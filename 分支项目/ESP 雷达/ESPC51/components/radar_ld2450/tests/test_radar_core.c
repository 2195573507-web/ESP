#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ld2450_parser.h"
#include "radar_edge_filter.h"
#include "radar_state_codec.h"

typedef struct {
    radar_frame_t frames[4];
    size_t count;
} capture_t;

static void capture_frame(const radar_frame_t *frame, void *ctx)
{
    capture_t *capture = ctx;
    assert(capture->count < 4U);
    capture->frames[capture->count++] = *frame;
}

static uint16_t directional(int16_t value)
{
    return value >= 0 ? (uint16_t)(32768 + value) : (uint16_t)(-value);
}

static void put_u16(uint8_t *out, uint16_t value)
{
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8U);
}

static void put_target(uint8_t *slot, int16_t x, int16_t y, int16_t speed, uint16_t resolution)
{
    put_u16(slot, directional(x));
    put_u16(slot + 2U, (uint16_t)(32768 + y));
    put_u16(slot + 4U, directional(speed));
    put_u16(slot + 6U, resolution);
}

static void make_frame(uint8_t frame[LD2450_FRAME_SIZE], uint8_t target_count)
{
    memset(frame, 0, LD2450_FRAME_SIZE);
    frame[0] = 0xAAU;
    frame[1] = 0xFFU;
    frame[2] = 0x03U;
    for (uint8_t i = 0U; i < target_count; ++i) {
        put_target(frame + LD2450_HEADER_SIZE + i * LD2450_TARGET_SIZE,
                   (int16_t)(100 + i), (int16_t)(1000 + i), (int16_t)(10 + i), 320U);
    }
    frame[28] = 0x55U;
    frame[29] = 0xCCU;
}

static void test_official_decode_and_streaming(void)
{
    uint8_t frame[LD2450_FRAME_SIZE];
    make_frame(frame, 0U);
    const uint8_t official[] = {0x0EU, 0x03U, 0xB1U, 0x86U, 0x10U, 0x00U, 0x40U, 0x01U};
    memcpy(frame + LD2450_HEADER_SIZE, official, sizeof(official));

    ld2450_parser_t parser;
    capture_t capture = {0};
    ld2450_parser_init(&parser);
    for (size_t i = 0U; i < sizeof(frame); ++i) {
        (void)ld2450_parser_feed(&parser, &frame[i], 1U, 100U + i, capture_frame, &capture);
    }
    assert(capture.count == 1U);
    const radar_target_t *target = &capture.frames[0].targets[0];
    assert(target->slot == 0U && target->x_mm == -782 && target->y_mm == 1713);
    assert(target->speed_cm_s == -16 && target->resolution_mm == 320U);
    assert(target->distance_mm == 1883U);

    uint8_t two_frames[LD2450_FRAME_SIZE * 2U];
    make_frame(two_frames, 3U);
    memcpy(two_frames + LD2450_FRAME_SIZE, two_frames, LD2450_FRAME_SIZE);
    capture = (capture_t){0};
    ld2450_parser_init(&parser);
    assert(ld2450_parser_feed(&parser, two_frames, sizeof(two_frames), 300U, capture_frame, &capture) == 2U);
    assert(capture.count == 2U && capture.frames[0].target_count == 3U);
    assert(capture.frames[0].targets[2].slot == 2U);
}

static void test_resync_and_zero_target_frame(void)
{
    uint8_t frame[LD2450_FRAME_SIZE];
    make_frame(frame, 0U);
    uint8_t stream[LD2450_FRAME_SIZE + 7U];
    memset(stream, 0x5AU, 7U);
    memcpy(stream + 7U, frame, sizeof(frame));
    ld2450_parser_t parser;
    capture_t capture = {0};
    ld2450_parser_init(&parser);
    (void)ld2450_parser_feed(&parser, stream, sizeof(stream), 500U, capture_frame, &capture);
    assert(capture.count == 1U && capture.frames[0].target_count == 0U);
    assert(parser.diagnostics.resync_count > 0U);

    ld2450_parser_init(&parser);
    (void)ld2450_parser_feed(&parser, frame, 15U, 600U, capture_frame, &capture);
    ld2450_parser_note_timeout(&parser, 2601U);
    assert(parser.length == 0U && parser.diagnostics.partial_timeouts == 1U);
}

static void test_v3_codec(void)
{
    radar_target_sample_t sample = {
        .local_id = 2U,
        .link_state = 5U,
        .sample_valid = true,
        .frame_seq = 234U,
        .frame_uptime_ms = 123450U,
        .target_count = 1U,
        .targets = {{.valid = true, .slot = 0U, .x_mm = 1200, .y_mm = 800,
                     .speed_cm_s = 15, .resolution_mm = 320U, .distance_mm = 1442U,
                     .confidence = 80U}},
    };
    char json[RADAR_RESULT_JSON_MAX_BYTES];
    const int length = radar_result_encode_json(&sample, 123456U, 10U, json, sizeof(json));
    assert(length > 0 && (size_t)length < sizeof(json));
    assert(strstr(json, "\"p\":3") != NULL);
    assert(strstr(json, "\"id\":2") != NULL);
    assert(strstr(json, "\"t\":\"radar\"") != NULL);
    assert(strstr(json, "\"q\":10") != NULL);
    assert(strstr(json, "\"device_id\":\"sensair_shuttle_02\"") != NULL);
    assert(strstr(json, "\"target_id\":1") != NULL);
    assert(strstr(json, "\"x_mm\":1200") != NULL);
    assert(strstr(json, "\"velocity_cm_s\":15") != NULL);
    assert(strstr(json, "\"confidence\":80") != NULL);

    sample.sample_valid = false;
    sample.target_count = 0U;
    assert(radar_result_encode_json(&sample, 123500U, 11U, json, sizeof(json)) > 0);
    sample.target_count = 1U;
    assert(radar_result_encode_json(&sample, 123500U, 12U, json, sizeof(json)) < 0);
}

static void test_edge_filter(void)
{
    radar_edge_filter_t filter;
    radar_edge_filter_init(&filter);
    radar_target_sample_t sample = {
        .local_id = 1U,
        .link_state = 5U,
        .sample_valid = true,
        .frame_seq = 1U,
        .target_count = 3U,
        .targets = {
            {.valid = true, .slot = 1U, .x_mm = 200, .y_mm = 0, .speed_cm_s = 30,
             .resolution_mm = 320U, .distance_mm = 2000U},
            {.valid = true, .slot = 0U, .x_mm = 100, .y_mm = 0, .speed_cm_s = 10,
             .resolution_mm = 320U, .distance_mm = 1000U},
            {.valid = true, .slot = 2U, .x_mm = 0, .y_mm = 0, .speed_cm_s = 20,
             .resolution_mm = 320U, .distance_mm = 6001U},
        },
    };
    radar_edge_filter_apply(&filter, &sample);
    assert(sample.target_count == 2U);
    assert(sample.targets[0].slot == 0U && sample.targets[0].confidence == 60U);
    assert(sample.targets[1].slot == 1U && sample.targets[1].confidence == 60U);

    sample.frame_seq = 2U;
    sample.target_count = 1U;
    sample.targets[0] = (radar_target_t){.valid = true, .slot = 0U, .x_mm = 100,
                                          .y_mm = 0, .speed_cm_s = 16,
                                          .resolution_mm = 320U, .distance_mm = 1000U};
    radar_edge_filter_apply(&filter, &sample);
    assert(sample.target_count == 1U && sample.targets[0].speed_cm_s == 12);
    assert(sample.targets[0].confidence == 70U);
}

int main(void)
{
    test_official_decode_and_streaming();
    test_resync_and_zero_target_frame();
    test_v3_codec();
    test_edge_filter();
    puts("C5 LD2450 parser, filter and v3 codec tests: PASS");
    return 0;
}
