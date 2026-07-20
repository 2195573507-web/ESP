#include "mic_vad.h"

#include <stddef.h>

void mic_vad_init(mic_vad_t *vad)
{
    if (vad == NULL) {
        return;
    }

    vad->state = MIC_VAD_STATE_IDLE;
    vad->start_count = 0;
    vad->end_count = 0;
    vad->speech_frames = 0;
}

mic_vad_event_t mic_vad_process(mic_vad_t *vad, const mic_vad_features_t *features)
{
    if (vad == NULL || features == NULL) {
        return MIC_VAD_EVENT_NONE;
    }

    mic_vad_event_t event = MIC_VAD_EVENT_NONE;
    uint32_t pcm_rms = features->pcm_rms;
    uint32_t pcm_peak = features->pcm_peak;

    switch (vad->state) {
    case MIC_VAD_STATE_IDLE:
        vad->speech_frames = 0;
        vad->end_count = 0;
        if (pcm_rms >= MIC_VAD_START_RMS && pcm_peak >= MIC_VAD_START_PEAK) {
            vad->start_count++;
            if (vad->start_count >= MIC_VAD_START_FRAMES) {
                vad->state = MIC_VAD_STATE_SPEECH;
                vad->speech_frames = 0;
                vad->end_count = 0;
                vad->start_count = 0;
                event = MIC_VAD_EVENT_VOICE_START;
            }
        } else {
            vad->start_count = 0;
        }
        break;

    case MIC_VAD_STATE_SPEECH:
        vad->speech_frames++;
        if (vad->speech_frames >= MIC_VAD_MAX_SPEECH_FRAMES) {
            vad->state = MIC_VAD_STATE_IDLE;
            vad->start_count = 0;
            vad->end_count = 0;
            event = MIC_VAD_EVENT_VOICE_END;
            break;
        }
        if (pcm_rms <= MIC_VAD_END_RMS) {
            vad->end_count++;
            vad->state = MIC_VAD_STATE_HANGOVER;
        } else {
            vad->end_count = 0;
        }
        break;

    case MIC_VAD_STATE_HANGOVER:
        vad->speech_frames++;
        if (vad->speech_frames >= MIC_VAD_MAX_SPEECH_FRAMES) {
            vad->state = MIC_VAD_STATE_IDLE;
            vad->start_count = 0;
            vad->end_count = 0;
            event = MIC_VAD_EVENT_VOICE_END;
            break;
        }
        if (pcm_rms <= MIC_VAD_END_RMS) {
            vad->end_count++;
            if (vad->end_count >= MIC_VAD_END_FRAMES) {
                if (vad->speech_frames >= MIC_VAD_MIN_SPEECH_FRAMES) {
                    event = MIC_VAD_EVENT_VOICE_END;
                }
                vad->state = MIC_VAD_STATE_IDLE;
                vad->start_count = 0;
                vad->end_count = 0;
                vad->speech_frames = 0;
            }
        } else {
            vad->state = MIC_VAD_STATE_SPEECH;
            vad->end_count = 0;
        }
        break;

    default:
        mic_vad_init(vad);
        break;
    }

    return event;
}
