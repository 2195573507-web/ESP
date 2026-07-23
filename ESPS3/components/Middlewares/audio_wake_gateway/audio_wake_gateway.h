#ifndef AUDIO_WAKE_GATEWAY_H
#define AUDIO_WAKE_GATEWAY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    uint8_t source_id;
    uint32_t wake_stream_id;
    int64_t detected_at_ms;
} audio_wake_event_t;

/* Called from the WakeNet receiver task. Implementations must not block. */
typedef esp_err_t (*audio_wake_gateway_event_handler_t)(const audio_wake_event_t *event,
                                                         void *context);

esp_err_t audio_wake_gateway_init(void);
esp_err_t audio_wake_gateway_set_event_handler(audio_wake_gateway_event_handler_t handler,
                                               void *context);
esp_err_t audio_wake_gateway_detect_pcm(const int16_t *samples,
                                        size_t sample_count,
                                        const char *device_id,
                                        bool *out_detected);

#endif
