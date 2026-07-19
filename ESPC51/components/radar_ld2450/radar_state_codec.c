#include "radar_state_codec.h"

#include <stdarg.h>
#include <stdio.h>

static bool append_text(char *out,
                        size_t out_size,
                        size_t *used,
                        const char *format,
                        ...)
{
    if (out == NULL || used == NULL || *used >= out_size) {
        return false;
    }

    va_list args;
    va_start(args, format);
    const int written = vsnprintf(out + *used, out_size - *used, format, args);
    va_end(args);
    if (written < 0 || (size_t)written >= out_size - *used) {
        return false;
    }
    *used += (size_t)written;
    return true;
}

int radar_result_encode_json(const radar_target_sample_t *sample,
                             uint32_t request_uptime_ms,
                             uint32_t request_sequence,
                             char *out,
                             size_t out_size)
{
    if (sample == NULL || out == NULL || out_size == 0U ||
        sample->local_id == 0U || sample->local_id > 2U ||
        request_sequence == 0U || sample->target_count > LD2450_MAX_TARGETS ||
        (!sample->sample_valid && sample->target_count != 0U) ||
        (sample->sample_valid && sample->frame_seq == 0U)) {
        return -1;
    }

    size_t used = 0U;
    if (!append_text(out,
                     out_size,
                     &used,
                     "{\"p\":3,\"id\":%u,\"t\":\"radar\",\"u\":%lu,\"q\":%lu,\"v\":{"
                     "\"device_id\":\"%s\",\"link_state\":%u,\"sample_valid\":%u,"
                     "\"frame_seq\":%lu,\"frame_uptime_ms\":%lu,\"target_count\":%u,\"targets\":[",
                     (unsigned int)sample->local_id,
                     (unsigned long)request_uptime_ms,
                     (unsigned long)request_sequence,
                     sample->local_id == 1U ? "sensair_shuttle_01" : "sensair_shuttle_02",
                     (unsigned int)sample->link_state,
                     sample->sample_valid ? 1U : 0U,
                     (unsigned long)sample->frame_seq,
                     (unsigned long)sample->frame_uptime_ms,
                     (unsigned int)sample->target_count)) {
        return -1;
    }

    for (uint8_t i = 0U; i < sample->target_count; ++i) {
        const radar_target_t *target = &sample->targets[i];
        if (!target->valid || target->slot >= LD2450_MAX_TARGETS) {
            return -1;
        }
        for (uint8_t prior = 0U; prior < i; ++prior) {
            if (sample->targets[prior].slot == target->slot) {
                return -1;
            }
        }
        if (!append_text(out,
                         out_size,
                         &used,
                         "%s{\"target_id\":%u,\"x_mm\":%d,\"y_mm\":%d,"
                         "\"velocity_cm_s\":%d,\"confidence\":%u,\"resolution_mm\":%u,"
                         "\"distance_mm\":%lu}",
                         i == 0U ? "" : ",",
                         (unsigned int)target->slot + 1U,
                         (int)target->x_mm,
                         (int)target->y_mm,
                         (int)target->speed_cm_s,
                         (unsigned int)target->confidence,
                         (unsigned int)target->resolution_mm,
                         (unsigned long)target->distance_mm)) {
            return -1;
        }
    }

    return append_text(out, out_size, &used, "]}}") ? (int)used : -1;
}
