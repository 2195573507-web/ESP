#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_lcd_panel_dev.h"
#include "esp_lcd_panel_io.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LCD_DRIVER_UNINITIALIZED = 0,
    LCD_DRIVER_ALLOCATING,
    LCD_DRIVER_READY,
    LCD_DRIVER_PUBLISHED,
    LCD_DRIVER_STOPPING,
    LCD_DRIVER_FAILED,
} lcd_driver_state_t;

typedef struct {
    lcd_driver_state_t state;
    size_t legacy_dma_bytes;
    size_t steady_dma_bytes;
    size_t internal_free;
    size_t internal_largest;
    size_t dma_free;
    size_t dma_largest;
    size_t psram_free;
    size_t psram_largest;
    bool spi_bus_owned;
    bool legacy_released;
} lcd_driver_metrics_t;

esp_err_t lcd_driver_start(void);
esp_err_t lcd_driver_register_lvgl(void);
esp_err_t lcd_driver_stop(void);
bool lcd_driver_is_ready(void);
lcd_driver_state_t lcd_driver_get_state(void);
lv_display_t *lcd_driver_get_display(void);
esp_lcd_panel_io_handle_t lcd_driver_get_io_handle(void);
esp_lcd_panel_handle_t lcd_driver_get_panel_handle(void);
void lcd_driver_get_metrics(lcd_driver_metrics_t *out_metrics);

/* Available before LVGL registration for manufacturing color verification. */
esp_err_t lcd_driver_fill_legacy(uint16_t rgb565);

#ifdef __cplusplus
}
#endif
