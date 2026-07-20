#include "radar_coordinate_transform.h"

#include <limits.h>
#include <math.h>
#include <string.h>

/*
 * 将 LD2450 的原始毫米坐标映射到安装坐标系：先镜像、再旋转、最后平移。
 * 房间边界判断仅使用变换后的坐标，保证分区和跟踪共享同一空间语义。
 */

#define RADAR_PI 3.14159265358979323846

static int32_t clamp_i32(double value)
{
    if (value > INT32_MAX) return INT32_MAX;
    if (value < INT32_MIN) return INT32_MIN;
    return (int32_t)lround(value);
}

bool radar_coordinate_transform_target(const radar_installation_config_t *config,
                                       const radar_target_t *raw,
                                       radar_spatial_target_t *out)
{
    /* The official demo suppresses a slot with zero distance resolution.  Keep
     * that validity boundary here as a second line of defense after parsing. */
    if (config == NULL || raw == NULL || out == NULL || !raw->valid ||
        raw->resolution_mm == 0U ||
        raw->x_mm == INT16_MIN || raw->x_mm == -32704 ||
        raw->y_mm == INT16_MIN || raw->y_mm == -32704) {
        return false;
    }
    double x = raw->x_mm;
    double y = raw->y_mm;
    if (config->flip_x) x = -x;
    if (config->flip_y) y = -y;

    const double radians = (double)config->rotation_deg * (RADAR_PI / 180.0);
    const double rotated_x = x * cos(radians) - y * sin(radians);
    const double rotated_y = x * sin(radians) + y * cos(radians);
    memset(out, 0, sizeof(*out));
    out->valid = true;
    out->x_mm = clamp_i32(rotated_x) + config->origin_offset_x_mm;
    out->y_mm = clamp_i32(rotated_y) + config->origin_offset_y_mm;
    out->speed_cm_s = raw->speed_cm_s;
    out->resolution_mm = raw->resolution_mm;
    const double distance = sqrt((double)out->x_mm * out->x_mm +
                                 (double)out->y_mm * out->y_mm);
    out->distance_mm = distance > UINT32_MAX ? UINT32_MAX : (uint32_t)lround(distance);
    out->angle_deg = (int16_t)lround(atan2((double)out->x_mm, (double)out->y_mm) *
                                     (180.0 / RADAR_PI));
    return config->max_detection_distance_mm == 0U ||
           out->distance_mm <= config->max_detection_distance_mm;
}

bool radar_coordinate_transform_in_room(const radar_installation_config_t *config,
                                        const radar_spatial_target_t *target)
{
    if (config == NULL || target == NULL || !target->valid) {
        return false;
    }
    const radar_rect_t *bounds = &config->room_bounds;
    return target->x_mm >= bounds->min_x_mm && target->x_mm <= bounds->max_x_mm &&
           target->y_mm >= bounds->min_y_mm && target->y_mm <= bounds->max_y_mm;
}
