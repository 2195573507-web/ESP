#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Initialize the CST816T on the shared BME I2C0 bus (GPIO2/GPIO3). */
esp_err_t cst816t_init(void);

/**
 * Read the current CST816T point state.
 *
 * The function returns ESP_OK for a successful controller read. When no
 * finger is present, *pressed is false and x/y are zero.
 */
esp_err_t cst816t_read_point(uint16_t *x, uint16_t *y, bool *pressed);

#ifdef __cplusplus
}
#endif
