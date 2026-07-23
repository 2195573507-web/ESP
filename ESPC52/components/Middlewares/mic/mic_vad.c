#include "mic_vad.h"

#include <stddef.h>

static const mic_vad_config_t s_wake_listener_config = {
    .start_rms = APP_VOICE_VAD_SPEECH_START_RMS,
    .start_peak = APP_VOICE_VAD_SPEECH_START_PEAK,
    .end_rms = APP_VOICE_VAD_SPEECH_END_RMS,
    .start_confirm_frames = APP_VOICE_VAD_START_FRAMES,
    .hangover_frames = (APP_VOICE_VAD_SILENCE_END_MS + MIC_VAD_FRAME_MS - 1U) / MIC_VAD_FRAME_MS,
    .max_active_frames = (APP_VOICE_VAD_MAX_RECORD_MS + MIC_VAD_FRAME_MS - 1U) / MIC_VAD_FRAME_MS,
    .max_active_ms = APP_VOICE_VAD_MAX_RECORD_MS,
    .source = "default",
};

static const mic_vad_config_t s_command_capture_config = {
    .start_rms = APP_VOICE_COMMAND_VAD_SPEECH_START_RMS,
    .start_peak = APP_VOICE_COMMAND_VAD_SPEECH_START_PEAK,
    .end_rms = APP_VOICE_COMMAND_VAD_SPEECH_END_RMS,
    .start_confirm_frames = APP_VOICE_COMMAND_VAD_START_FRAMES,
    .hangover_frames = (APP_VOICE_COMMAND_VAD_SILENCE_END_MS + MIC_VAD_FRAME_MS - 1U) /
                       MIC_VAD_FRAME_MS,
    .max_active_frames = (APP_VOICE_COMMAND_VAD_MAX_RECORD_MS + MIC_VAD_FRAME_MS - 1U) /
                         MIC_VAD_FRAME_MS,
    .max_active_ms = APP_VOICE_COMMAND_VAD_MAX_RECORD_MS,
    .source = "default",
};

static void mic_vad_reset_to_idle(mic_vad_t *vad)
{
    vad->state = MIC_VAD_STATE_IDLE;
    vad->start_count = 0;
    vad->end_count = 0;
    vad->speech_frames = 0;
}

static void mic_vad_finish(mic_vad_t *vad, mic_vad_end_reason_t reason)
{
    vad->end_reason = reason;
    mic_vad_reset_to_idle(vad);
}

const char *mic_vad_end_reason_name(mic_vad_end_reason_t reason)
{
    switch (reason) {
    case MIC_VAD_END_REASON_SILENCE:
        return "silence";
    case MIC_VAD_END_REASON_TIMEOUT:
        return "timeout";
    case MIC_VAD_END_REASON_NONE:
    default:
        return "none";
    }
}

const char *mic_vad_mode_name(mic_vad_mode_t mode)
{
    return mode == MIC_VAD_MODE_COMMAND_CAPTURE ? "command_capture" : "wake_listener";
}

void mic_vad_init(mic_vad_t *vad)
{
    mic_vad_init_mode(vad, MIC_VAD_MODE_WAKE_LISTENER);
}

void mic_vad_init_mode(mic_vad_t *vad, mic_vad_mode_t mode)
{
    if (vad == NULL) {
        return;
    }

    vad->mode = mode;
    vad->config = mode == MIC_VAD_MODE_COMMAND_CAPTURE ?
                      s_command_capture_config : s_wake_listener_config;
    vad->end_reason = MIC_VAD_END_REASON_NONE;
    mic_vad_reset_to_idle(vad);
}

mic_vad_event_t mic_vad_process(mic_vad_t *vad, const mic_vad_features_t *features)
{
    if (vad == NULL || features == NULL) {
        return MIC_VAD_EVENT_NONE;
    }

    mic_vad_event_t event = MIC_VAD_EVENT_NONE;
    const uint32_t pcm_rms = features->pcm_rms;
    const uint32_t pcm_peak = features->pcm_peak;
    vad->end_reason = MIC_VAD_END_REASON_NONE;

    switch (vad->state) {
    case MIC_VAD_STATE_IDLE:
        vad->speech_frames = 0;
        vad->end_count = 0;
        if (pcm_rms >= vad->config.start_rms && pcm_peak >= vad->config.start_peak) {
            vad->start_count++;
            if (vad->start_count >= (int)vad->config.start_confirm_frames) {
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
        if (vad->speech_frames >= (int)vad->config.max_active_frames) {
            /* This is a configured failsafe timeout, not the normal end of a
             * speech segment. Normal completion always follows real silence. */
            mic_vad_finish(vad, MIC_VAD_END_REASON_TIMEOUT);
            event = MIC_VAD_EVENT_VOICE_END;
            break;
        }
        if (pcm_rms <= vad->config.end_rms) {
            vad->end_count++;
            vad->state = MIC_VAD_STATE_HANGOVER;
        } else {
            vad->end_count = 0;
        }
        break;

    case MIC_VAD_STATE_HANGOVER:
        vad->speech_frames++;
        if (vad->speech_frames >= (int)vad->config.max_active_frames) {
            mic_vad_finish(vad, MIC_VAD_END_REASON_TIMEOUT);
            event = MIC_VAD_EVENT_VOICE_END;
            break;
        }
        if (pcm_rms <= vad->config.end_rms) {
            vad->end_count++;
            if (vad->end_count >= (int)vad->config.hangover_frames) {
                /* Once VOICE_START was emitted, always emit exactly one
                 * matching VOICE_END so stream owners cannot remain open. */
                mic_vad_finish(vad, MIC_VAD_END_REASON_SILENCE);
                event = MIC_VAD_EVENT_VOICE_END;
            }
        } else {
            /* Hysteresis: renewed speech cancels pending end without emitting
             * an event, so the active stream continues uninterrupted. */
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
