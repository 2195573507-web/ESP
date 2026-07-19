#include <assert.h>
#include <stdint.h>
#include <string.h>

#include "radar_ble_stream.h"

int main(void)
{
    radar_ble_stream_t stream;
    uint8_t input[600];
    uint8_t output[512];
    memset(input, 0x5a, sizeof(input));
    radar_ble_stream_init(&stream);
    assert(radar_ble_stream_push(&stream, input, sizeof(input)) == 512U);
    assert(stream.overflow_count == 1U);
    assert(stream.resync_count == 1U);
    assert(radar_ble_stream_take(&stream, output, sizeof(output)) == 512U);
    assert(stream.length == 0U);
    assert(stream.notify_count == 1U);
    assert(stream.notify_bytes == 600U);
    return 0;
}
