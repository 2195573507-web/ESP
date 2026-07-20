#include "radar_zone_map.h"

#include <string.h>

/*
 * 分区映射只解析已启用的矩形区域。上一次分区作为稳定性提示传入，
 * 从而在边界附近优先维持既有归属，减少目标轻微抖动导致的区域跳变。
 */

static const radar_zone_definition_t *find_zone(const radar_zone_map_t *map, uint8_t zone_id)
{
    if (map == NULL || map->installation == NULL || zone_id == 0U) return NULL;
    for (uint8_t i = 0U; i < map->installation->zone_count && i < RADAR_ZONE_MAP_MAX_ZONES; ++i) {
        const radar_zone_definition_t *zone = &map->installation->zones[i];
        if (zone->enabled && zone->zone_id == zone_id) return zone;
    }
    return NULL;
}

void radar_zone_map_init(radar_zone_map_t *map,
                         const radar_installation_config_t *installation)
{
    if (map != NULL) {
        map->installation = installation;
    }
}

bool radar_zone_map_rect_contains(const radar_rect_t *rect,
                                  int32_t x_mm,
                                  int32_t y_mm,
                                  uint32_t hysteresis_mm)
{
    if (rect == NULL) return false;
    const int64_t h = hysteresis_mm;
    return (int64_t)x_mm >= (int64_t)rect->min_x_mm - h &&
           (int64_t)x_mm <= (int64_t)rect->max_x_mm + h &&
           (int64_t)y_mm >= (int64_t)rect->min_y_mm - h &&
           (int64_t)y_mm <= (int64_t)rect->max_y_mm + h;
}

bool radar_zone_map_resolve(const radar_zone_map_t *map,
                            int32_t x_mm,
                            int32_t y_mm,
                            uint8_t previous_zone_id,
                            uint8_t *out_zone_id,
                            radar_zone_type_t *out_zone_type)
{
    if (out_zone_id != NULL) *out_zone_id = 0U;
    if (out_zone_type != NULL) *out_zone_type = RADAR_ZONE_NONE;
    if (map == NULL || map->installation == NULL) return false;
    const radar_installation_config_t *installation = map->installation;
    const radar_rect_t *room = &installation->room_bounds;
    if (!radar_zone_map_rect_contains(room, x_mm, y_mm, 0U)) return false;

    for (uint8_t i = 0U; i < installation->zone_count && i < RADAR_ZONE_MAP_MAX_ZONES; ++i) {
        const radar_zone_definition_t *zone = &installation->zones[i];
        if (zone->enabled && zone->type == RADAR_ZONE_IGNORE &&
            radar_zone_map_rect_contains(&zone->rect, x_mm, y_mm, 0U)) return false;
    }

    const radar_zone_definition_t *previous = find_zone(map, previous_zone_id);
    if (previous != NULL && previous->type != RADAR_ZONE_IGNORE &&
        radar_zone_map_rect_contains(&previous->rect, x_mm, y_mm, previous->hysteresis_mm)) {
        if (out_zone_id != NULL) *out_zone_id = previous->zone_id;
        if (out_zone_type != NULL) *out_zone_type = previous->type;
        return true;
    }
    for (uint8_t i = 0U; i < installation->zone_count && i < RADAR_ZONE_MAP_MAX_ZONES; ++i) {
        const radar_zone_definition_t *zone = &installation->zones[i];
        if (zone->enabled && zone->type != RADAR_ZONE_IGNORE &&
            radar_zone_map_rect_contains(&zone->rect, x_mm, y_mm, 0U)) {
            if (out_zone_id != NULL) *out_zone_id = zone->zone_id;
            if (out_zone_type != NULL) *out_zone_type = zone->type;
            return true;
        }
    }
    return true;
}
