#ifndef RADAR_ZONE_MAP_H
#define RADAR_ZONE_MAP_H

#include <stdbool.h>

#include "radar_spatial_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    /* 安装配置由空间状态机持有，映射器只借用该只读指针。 */
    const radar_installation_config_t *installation;
} radar_zone_map_t;

void radar_zone_map_init(radar_zone_map_t *map,
                         const radar_installation_config_t *installation);
bool radar_zone_map_resolve(const radar_zone_map_t *map,
                            int32_t x_mm,
                            int32_t y_mm,
                            uint8_t previous_zone_id,
                            uint8_t *out_zone_id,
                            radar_zone_type_t *out_zone_type);
bool radar_zone_map_rect_contains(const radar_rect_t *rect,
                                  int32_t x_mm,
                                  int32_t y_mm,
                                  uint32_t hysteresis_mm);

#ifdef __cplusplus
}
#endif

#endif
