#ifndef RADAR_LOG_MANAGER_H
#define RADAR_LOG_MANAGER_H

#include "esp_err.h"
#include "radar_rate_manager.h"
#include "radar_registry.h"
#include "radar_spatial_types.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t radar_log_manager_start(void);
esp_err_t radar_log_manager_stop(void);
void radar_log_manager_publish(const RadarSourceContext *context,
                               const radar_rate_manager_t *rate_manager);
void radar_log_manager_publish_stack_words(uint32_t free_words);

#ifdef __cplusplus
}
#endif

#endif
