#ifndef C5_AUDIO_TRANSPORT_H
#define C5_AUDIO_TRANSPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t c5_audio_transport_init(uint8_t source_id);
esp_err_t c5_audio_transport_start(void);
esp_err_t c5_audio_transport_begin_pre_roll(uint32_t frame_count);
esp_err_t c5_audio_transport_append_pcm(const int16_t *pcm, size_t samples);
esp_err_t c5_audio_transport_mark_live(void);
esp_err_t c5_audio_transport_mark_tail(void);
esp_err_t c5_audio_transport_finish(void);
esp_err_t c5_audio_transport_abort(const char *reason);
bool c5_audio_transport_is_idle(void);
uint32_t c5_audio_transport_get_stream_id(void);

#endif
