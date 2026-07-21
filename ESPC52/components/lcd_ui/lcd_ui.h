#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "lcd_service.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef esp_err_t (*lcd_ui_wake_request_fn)(void *user_ctx);

/* Must run after lvgl_port_init() and before the first lv_display_create(). */
esp_err_t lcd_ui_prepare_lvgl_pool(void *user_ctx);
void lcd_ui_release_lvgl_pool(void *user_ctx);
esp_err_t lcd_ui_start(lv_display_t *display, lcd_ui_wake_request_fn wake_request, void *wake_ctx);
esp_err_t lcd_ui_stop(void);
void lcd_ui_apply(const lcd_system_snapshot_t *snapshot,
                  const lcd_command_t *command,
                  bool command_visible,
                  bool full_refresh);

#ifdef __cplusplus
}
#endif
