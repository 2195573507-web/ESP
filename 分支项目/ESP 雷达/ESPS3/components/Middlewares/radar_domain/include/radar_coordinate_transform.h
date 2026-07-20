#ifndef RADAR_COORDINATE_TRANSFORM_H
#define RADAR_COORDINATE_TRANSFORM_H

#include <stdbool.h>

#include "radar_spatial_types.h"

#ifdef __cplusplus
extern "C" {
#endif

bool radar_coordinate_transform_target(const radar_installation_config_t *config,
                                       const radar_target_t *raw,
                                       radar_spatial_target_t *out);
bool radar_coordinate_transform_in_room(const radar_installation_config_t *config,
                                        const radar_spatial_target_t *target);

#ifdef __cplusplus
}
#endif

#endif
