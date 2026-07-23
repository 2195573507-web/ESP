#ifndef C5_AUDIO_TRANSPORT_H
#define C5_AUDIO_TRANSPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

esp_err_t c5_audio_transport_init(uint8_t source_id);
esp_err_t c5_audio_transport_start(void);
/* Queue the diagnostic pre-roll boundary before its PCM frames.  The sender
 * owns sequence assignment, so this boundary also establishes wire order. */
esp_err_t c5_audio_transport_begin_pre_roll(uint32_t frame_count);
esp_err_t c5_audio_transport_append_pcm(const int16_t *pcm, size_t samples);
/* Queue PRE_ROLL_END/LIVE_BEGIN before any live PCM can enter the wire queue. */
esp_err_t c5_audio_transport_mark_live(void);
esp_err_t c5_audio_transport_mark_tail(void);
esp_err_t c5_audio_transport_finish(void);
esp_err_t c5_audio_transport_abort(const char *reason);
bool c5_audio_transport_is_idle(void);
uint32_t c5_audio_transport_get_stream_id(void);

#endif
