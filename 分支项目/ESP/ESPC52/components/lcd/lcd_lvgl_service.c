#include "lcd_service.h"

#include "lcd.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

#define LCD_LVGL_DRAW_BUFFER_LINES 10U
#define LCD_LVGL_DRAW_BUFFER_PIXELS (LCD_H_RES * LCD_LVGL_DRAW_BUFFER_LINES)
#define LCD_LVGL_TASK_PRIORITY 1
#define LCD_LVGL_TASK_STACK 4096
#define LCD_LVGL_TIMER_PERIOD_MS 20

static const char *TAG = "LCD_LVGL";
static lv_display_t *s_display;
static bool s_started;

static esp_err_t lcd_lvgl_create_status_ui(void)
{
    if (!lvgl_port_lock(1000)) {
        return ESP_ERR_TIMEOUT;
    }

    lv_obj_t *screen = lv_display_get_screen_active(s_display);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x101820), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);

    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, "SensAir C5");
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 12, 16);

    lv_obj_t *status = lv_label_create(screen);
    lv_label_set_text(status, "LCD + LVGL ready");
    lv_obj_set_style_text_color(status, lv_color_hex(0x79D2A6), LV_PART_MAIN);
    lv_obj_align(status, LV_ALIGN_TOP_LEFT, 12, 52);

    lv_obj_t *detail = lv_label_create(screen);
    lv_label_set_text(detail, "BME / CSI / voice remain independent");
    lv_obj_set_style_text_color(detail, lv_color_hex(0xB8C5D1), LV_PART_MAIN);
    lv_obj_set_width(detail, LCD_H_RES - 24);
    lv_label_set_long_mode(detail, LV_LABEL_LONG_WRAP);
    lv_obj_align(detail, LV_ALIGN_TOP_LEFT, 12, 88);

    lvgl_port_unlock();
    return ESP_OK;
}

esp_err_t lcd_service_start(void)
{
    if (s_started) {
        return ESP_OK;
    }

    esp_err_t ret = lcd_init();
    if (ret != ESP_OK) {
        return ret;
    }

    const lvgl_port_cfg_t lvgl_cfg = {
        .task_priority = LCD_LVGL_TASK_PRIORITY,
        .task_stack = LCD_LVGL_TASK_STACK,
        .task_affinity = -1,
        .task_max_sleep_ms = 1000,
        .task_stack_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT,
        .timer_period_ms = LCD_LVGL_TIMER_PERIOD_MS,
    };
    ret = lvgl_port_init(&lvgl_cfg);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = lcd_release_legacy_draw_buffer();
    if (ret != ESP_OK) {
        (void)lvgl_port_deinit();
        return ret;
    }

    const lvgl_port_display_cfg_t display_cfg = {
        .io_handle = lcd_get_io_handle(),
        .panel_handle = lcd_get_panel_handle(),
        .control_handle = NULL,
        .buffer_size = LCD_LVGL_DRAW_BUFFER_PIXELS,
        .double_buffer = false,
        .trans_size = 0,
        .hres = LCD_H_RES,
        .vres = LCD_V_RES,
        .monochrome = false,
        .rotation = {
            .swap_xy = LCD_SWAP_XY_ENABLE,
            .mirror_x = LCD_MIRROR_X_ENABLE,
            .mirror_y = LCD_MIRROR_Y_ENABLE,
        },
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = {
            .buff_dma = true,
            .buff_spiram = false,
            .sw_rotate = false,
            .swap_bytes = LCD_COLOR_SWAP_BYTES != 0,
            .full_refresh = false,
            .direct_mode = false,
        },
    };

    s_display = lvgl_port_add_disp(&display_cfg);
    if (s_display == NULL) {
        (void)lvgl_port_deinit();
        return ESP_ERR_NO_MEM;
    }

    ret = lcd_lvgl_create_status_ui();
    if (ret != ESP_OK) {
        (void)lvgl_port_remove_disp(s_display);
        s_display = NULL;
        (void)lvgl_port_deinit();
        return ret;
    }

    s_started = true;
    ESP_LOGI(TAG, "started with %u-byte single DMA draw buffer",
             (unsigned int)(LCD_LVGL_DRAW_BUFFER_PIXELS * sizeof(uint16_t)));
    return ESP_OK;
}

bool lcd_service_is_started(void)
{
    return s_started;
}