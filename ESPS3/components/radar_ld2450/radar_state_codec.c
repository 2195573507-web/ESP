#include "radar_state_codec.h"

#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>

/* 将本地快照编码为固定 schema 的 JSON；字段由网关接入协议定义，不能随意增删。 */

#include "radar_presence.h"

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
    int written = vsnprintf(out + *used, out_size - *used, format, args);
    va_end(args);
    if (written < 0 || (size_t)written >= out_size - *used) {
        return false;
    }
    *used += (size_t)written;
    return true;
}

int radar_state_encode_json(const radar_snapshot_t *snapshot,
                            uint8_t local_id,
                            uint32_t sequence,
                            uint64_t uptime_ms,
                            char *out,
                            size_t out_size)
{
    if (snapshot == NULL || out == NULL || out_size == 0U ||
        local_id == 0U || local_id > 2U ||
        snapshot->current_target_count > LD2450_MAX_TARGETS) {
        return -1;
    }

    const uint64_t last_motion_age_ms =
        snapshot->last_motion_ms > 0U && uptime_ms >= snapshot->last_motion_ms ?
            uptime_ms - snapshot->last_motion_ms :
            UINT32_MAX;
    size_t used = 0U;
    if (!append_text(out,
                     out_size,
                     &used,
                     "{\"schema_version\":1,\"local_id\":%u,"
                     "\"sequence\":%lu,\"uptime_ms\":%llu,"
                     "\"state\":\"%s\",\"target_count\":%u,"
                     "\"uart_online\":%s,\"frame_fresh\":%s,"
                     "\"last_motion_age_ms\":%llu,\"targets\":[",
                     (unsigned int)local_id,
                     (unsigned long)sequence,
                     (unsigned long long)uptime_ms,
                     radar_presence_state_name(snapshot->state),
                     (unsigned int)snapshot->current_target_count,
                     snapshot->uart_online ? "true" : "false",
                     snapshot->frame_fresh ? "true" : "false",
                     (unsigned long long)last_motion_age_ms)) {
        return -1;
    }

    uint8_t emitted = 0U;
    for (size_t i = 0; i < LD2450_MAX_TARGETS; ++i) {
        const radar_target_t *target = &snapshot->targets[i];
        if (!target->valid) {
            continue;
        }
        if (emitted >= snapshot->current_target_count) {
            return -1;
        }
        if (!append_text(out,
                         out_size,
                         &used,
                         "%s{\"x_mm\":%d,\"y_mm\":%d,"
                         "\"speed_cm_s\":%d,\"resolution_mm\":%u}",
                         emitted > 0U ? "," : "",
                         (int)target->x_mm,
                         (int)target->y_mm,
                         (int)target->speed_cm_s,
                         (unsigned int)target->resolution_mm)) {
            return -1;
        }
        ++emitted;
    }
    if (emitted != snapshot->current_target_count ||
        !append_text(out, out_size, &used, "]}")) {
        return -1;
    }
    return (int)used;
}
