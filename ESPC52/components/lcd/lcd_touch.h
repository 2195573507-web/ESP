#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint16_t x;
    uint16_t y;
    bool pressed;
    uint32_t generation;
} lcd_touch_point_t;

/* The touch task only reads the already-owned I2C0 bus and writes this cache. */
esp_err_t lcd_touch_start(void);
esp_err_t lcd_touch_stop(void);
bool lcd_touch_is_available(void);
void lcd_touch_get_latest(lcd_touch_point_t *out_point);

#ifdef __cplusplus
}
#endif
