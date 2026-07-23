#include "lcd_ui.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "lcd_assets.h"
#include "lcd_board_profile.h"
#include "lcd_fault_injection.h"
#include "lcd_touch.h"

#define LCD_UI_ARENA_BYTES (96U * 1024U)
#define LCD_UI_POOL_OFFSET_BYTES 4096U
#define LCD_UI_POOL_BYTES (LCD_UI_ARENA_BYTES - LCD_UI_POOL_OFFSET_BYTES)
#define LCD_UI_ANIMATION_MS 100U
#define LCD_UI_BOOT_CHECK_MS 100U
#define LCD_UI_BOOT_MIN_MS 3000U

#define LCD_UI_BG_COLOR 0x101820
#define LCD_UI_TITLE_COLOR 0xFFFFFF
#define LCD_UI_ACCENT_COLOR 0x79D2A6
#define LCD_UI_VALUE_COLOR 0xDCE7EF
#define LCD_UI_INIT_COLOR 0xE5B75B
#define LCD_UI_ERROR_COLOR 0xE56B6F

static const char *TAG = "lcd_ui";

typedef struct {
    uint8_t *arena;
    lv_mem_pool_t lvgl_pool;
    lv_display_t *display;
    lv_indev_t *touch_indev;
    lv_timer_t *boot_timer;
    lv_timer_t *animation_timer;
    lv_obj_t *dashboard;
    lv_obj_t *boot_page;
    lv_obj_t *voice_overlay;
    lv_obj_t *command_overlay;
    lv_obj_t *cat;
    lv_obj_t *cat_hit_area;
    lv_obj_t *voice_cat;
    lv_obj_t *temperature_label;
    lv_obj_t *humidity_label;
    lv_obj_t *pressure_label;
    lv_obj_t *gas_label;
    lv_obj_t *air_label;
    lv_obj_t *network_label;
    lv_obj_t *cat_state_label;
    lv_obj_t *air_status_dot;
    lv_obj_t *network_status_dot;
    lv_obj_t *boot_display_label;
    lv_obj_t *boot_sensor_label;
    lv_obj_t *boot_network_label;
    lv_obj_t *boot_audio_label;
    lv_obj_t *boot_display_dot;
    lv_obj_t *boot_sensor_dot;
    lv_obj_t *boot_network_dot;
    lv_obj_t *boot_audio_dot;
    lv_obj_t *boot_progress;
    lv_obj_t *command_label;
    lcd_ui_wake_request_fn wake_request;
    void *wake_ctx;
    lcd_system_snapshot_t snapshot;
    uint32_t animation_frame;
    uint32_t boot_started_tick;
    bool boot_complete;
    char temperature_text[32];
    char humidity_text[32];
    char pressure_text[32];
    char gas_text[32];
    char air_text[32];
    char network_text[32];
    char cat_state_text[24];
    char boot_display_text[24];
    char boot_sensor_text[24];
    char boot_network_text[24];
    char boot_audio_text[24];
    char command_text[LCD_COMMAND_TITLE_MAX + LCD_COMMAND_TEXT_MAX + 8U];
} lcd_ui_context_t;

static lcd_ui_context_t *s_ui;

_Static_assert(sizeof(lcd_ui_context_t) <= 4096U, "UI control block must leave the PSRAM arena intact");
_Static_assert((LCD_UI_POOL_OFFSET_BYTES % sizeof(void *)) == 0U,
               "LVGL PSRAM pool must preserve heap allocation alignment");

static lv_obj_t *lcd_ui_label(lv_obj_t *parent, const char *text, int16_t x, int16_t y, int16_t width, lv_color_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    if (label == NULL) {
        return NULL;
    }
    lv_label_set_text_static(label, text);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_width(label, width);
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_color(label, color, LV_PART_MAIN);
    return label;
}

static lv_obj_t *lcd_ui_status_dot(lv_obj_t *parent, int16_t x, int16_t y, lv_color_t color)
{
    lv_obj_t *dot = lv_obj_create(parent);
    if (dot == NULL) {
        return NULL;
    }
    lv_obj_remove_style_all(dot);
    lv_obj_set_pos(dot, x, y);
    lv_obj_set_size(dot, 6, 6);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(dot, color, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, LV_PART_MAIN);
    return dot;
}

static void lcd_ui_set_text(lv_obj_t *label, const char *text)
{
    if (label != NULL && text != NULL) {
        lv_label_set_text_static(label, text);
        lv_obj_invalidate(label);
    }
}

static const char *lcd_ui_voice_name(lcd_voice_state_t state)
{
    switch (state) {
    case LCD_VOICE_WAKE: return "WAKE";
    case LCD_VOICE_RECORDING: return "RECORDING";
    case LCD_VOICE_WAITING: return "WAITING";
    case LCD_VOICE_PLAYING: return "PLAYING";
    case LCD_VOICE_ERROR: return "ERROR";
    case LCD_VOICE_IDLE:
    default: return "IDLE";
    }
}

static lv_color_t lcd_ui_network_color(const lcd_system_snapshot_t *snapshot)
{
    if (snapshot->wifi_state == LCD_WIFI_ONLINE && snapshot->gateway_online) {
        return lv_color_hex(LCD_UI_ACCENT_COLOR);
    }
    if (snapshot->wifi_state == LCD_WIFI_CONNECTING || snapshot->wifi_state == LCD_WIFI_ONLINE) {
        return lv_color_hex(LCD_UI_INIT_COLOR);
    }
    return lv_color_hex(LCD_UI_ERROR_COLOR);
}

static void lcd_ui_set_boot_stage(lv_obj_t *label,
                                  lv_obj_t *dot,
                                  char *text,
                                  size_t text_size,
                                  const char *name,
                                  bool ready)
{
    if (text == NULL || text_size == 0U) {
        return;
    }
    (void)snprintf(text, text_size, "%s %s", name, ready ? "READY" : "INIT");
    const lv_color_t color = lv_color_hex(ready ? LCD_UI_ACCENT_COLOR : LCD_UI_INIT_COLOR);
    lcd_ui_set_text(label, text);
    if (label != NULL) {
        lv_obj_set_style_text_color(label, color, LV_PART_MAIN);
    }
    if (dot != NULL) {
        lv_obj_set_style_bg_color(dot, color, LV_PART_MAIN);
    }
}

static void lcd_ui_touch_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    lcd_touch_point_t point = {0};
    lcd_touch_get_latest(&point);
    if (!point.pressed) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }
    int32_t x = point.x;
    int32_t y = point.y;
#if LCD_BOARD_SWAP_XY
    const int32_t swap = x;
    x = y;
    y = swap;
#endif
#if LCD_BOARD_MIRROR_X
    x = (int32_t)LCD_BOARD_HRES - 1 - x;
#endif
#if LCD_BOARD_MIRROR_Y
    y = (int32_t)LCD_BOARD_VRES - 1 - y;
#endif
    if (x < 0 || y < 0 || x >= (int32_t)LCD_BOARD_HRES || y >= (int32_t)LCD_BOARD_VRES) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }
    data->point.x = x;
    data->point.y = y;
    data->state = LV_INDEV_STATE_PRESSED;
}

static void lcd_ui_cat_click(lv_event_t *event)
{
    lcd_ui_context_t *const ui = lv_event_get_user_data(event);
    if (ui != NULL && ui->wake_request != NULL) {
        const esp_err_t ret = ui->wake_request(ui->wake_ctx);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "LCD_WAKE_EVENT_REJECTED ret=%s", esp_err_to_name(ret));
        }
    }
}

static void lcd_ui_boot_timer(lv_timer_t *timer)
{
    lcd_ui_context_t *const ui = timer != NULL ? lv_timer_get_user_data(timer) : NULL;
    if (ui == NULL || ui->boot_page == NULL) {
        return;
    }
    const bool display_ready = true;
    const bool sensor_ready = ui->snapshot.bme_valid;
    const bool network_ready = ui->snapshot.wifi_state == LCD_WIFI_ONLINE && ui->snapshot.gateway_online;
    const bool audio_ready = ui->snapshot.generation != 0U && ui->snapshot.voice_state != LCD_VOICE_ERROR;
    lcd_ui_set_boot_stage(ui->boot_display_label, ui->boot_display_dot,
                          ui->boot_display_text, sizeof(ui->boot_display_text), "Display", display_ready);
    lcd_ui_set_boot_stage(ui->boot_sensor_label, ui->boot_sensor_dot,
                          ui->boot_sensor_text, sizeof(ui->boot_sensor_text), "Sensor", sensor_ready);
    lcd_ui_set_boot_stage(ui->boot_network_label, ui->boot_network_dot,
                          ui->boot_network_text, sizeof(ui->boot_network_text), "Network", network_ready);
    lcd_ui_set_boot_stage(ui->boot_audio_label, ui->boot_audio_dot,
                          ui->boot_audio_text, sizeof(ui->boot_audio_text), "Audio", audio_ready);
    if (ui->boot_progress != NULL) {
        const uint8_t ready_count = (display_ready ? 1U : 0U) + (sensor_ready ? 1U : 0U) +
                                    (network_ready ? 1U : 0U) + (audio_ready ? 1U : 0U);
        lv_bar_set_value(ui->boot_progress, ready_count * 25U, LV_ANIM_OFF);
    }
    const uint32_t elapsed_ms = lv_tick_elaps(ui->boot_started_tick);
    if (ui->boot_complete && ui->boot_progress != NULL) {
        lv_bar_set_value(ui->boot_progress, 100, LV_ANIM_OFF);
    }
    if (ui->boot_complete && elapsed_ms >= LCD_UI_BOOT_MIN_MS) {
        lv_obj_add_flag(ui->boot_page, LV_OBJ_FLAG_HIDDEN);
        lv_timer_delete(timer);
        ui->boot_timer = NULL;
    }
}

static void lcd_ui_animation_timer(lv_timer_t *timer)
{
    lcd_ui_context_t *const ui = timer != NULL ? lv_timer_get_user_data(timer) : NULL;
    if (ui == NULL || ui->cat == NULL) {
        return;
    }
    ++ui->animation_frame;
    const bool dashboard_active = ui->snapshot.voice_state == LCD_VOICE_IDLE;
    if (dashboard_active) {
        lv_image_set_src(ui->cat, &lcd_asset_cat_idle);
        lv_obj_set_y(ui->cat, 180 + ((ui->animation_frame & 1U) ? -1 : 0));
        return;
    }
    if (ui->voice_cat == NULL) {
        return;
    }
    const bool speaking = ui->snapshot.speaker_active || ui->snapshot.voice_state == LCD_VOICE_PLAYING;
    const bool recording = ui->snapshot.voice_state == LCD_VOICE_RECORDING;
    const bool open_mouth = (ui->animation_frame / 4U) & 1U;
    lv_image_set_src(ui->voice_cat,
                     (speaking || recording) && open_mouth ? &lcd_asset_cat_talk_voice : &lcd_asset_cat_idle_voice);
    lv_obj_set_x(ui->voice_cat, 56 + (recording && (ui->animation_frame & 1U) ? 1 : 0));
}

static esp_err_t lcd_ui_create_touch(lcd_ui_context_t *ui)
{
    const esp_err_t touch_ret = lcd_touch_start();
    if (touch_ret != ESP_OK) {
        ESP_LOGW(TAG, "LCD_TOUCH_DEGRADED no touch input: %s", esp_err_to_name(touch_ret));
        return ESP_OK;
    }
    ui->touch_indev = lv_indev_create();
    if (ui->touch_indev == NULL) {
        (void)lcd_touch_stop();
        return ESP_ERR_NO_MEM;
    }
    lv_indev_set_type(ui->touch_indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(ui->touch_indev, lcd_ui_touch_read);
    lv_indev_set_display(ui->touch_indev, ui->display);
    return ESP_OK;
}

static esp_err_t lcd_ui_create(lcd_ui_context_t *ui)
{
    lv_obj_t *const screen = lv_display_get_screen_active(ui->display);
    if (screen == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    lv_obj_set_style_bg_color(screen, lv_color_hex(LCD_UI_BG_COLOR), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);

    ui->dashboard = lv_obj_create(screen);
    if (ui->dashboard == NULL) goto no_mem;
    lv_obj_remove_style_all(ui->dashboard);
    lv_obj_set_size(ui->dashboard, LCD_BOARD_HRES, LCD_BOARD_VRES);
    lv_obj_clear_flag(ui->dashboard, LV_OBJ_FLAG_SCROLLABLE);
    if (lcd_ui_label(ui->dashboard, "SensAir C5", 12, 12, 144, lv_color_hex(LCD_UI_TITLE_COLOR)) == NULL ||
        lcd_ui_label(ui->dashboard, "ENVIRONMENT", 12, 44, 144, lv_color_hex(LCD_UI_ACCENT_COLOR)) == NULL) goto no_mem;
    ui->temperature_label = lcd_ui_label(ui->dashboard, ui->temperature_text, 12, 72, 144, lv_color_hex(LCD_UI_VALUE_COLOR));
    ui->humidity_label = lcd_ui_label(ui->dashboard, ui->humidity_text, 12, 99, 144, lv_color_hex(LCD_UI_VALUE_COLOR));
    ui->pressure_label = lcd_ui_label(ui->dashboard, ui->pressure_text, 12, 126, 144, lv_color_hex(LCD_UI_VALUE_COLOR));
    ui->gas_label = lcd_ui_label(ui->dashboard, ui->gas_text, 12, 153, 144, lv_color_hex(LCD_UI_VALUE_COLOR));
    ui->air_label = lcd_ui_label(ui->dashboard, ui->air_text, 12, 182, 144, lv_color_hex(LCD_UI_ACCENT_COLOR));
    ui->network_label = lcd_ui_label(ui->dashboard, ui->network_text, 98, 253, 100, lv_color_hex(LCD_UI_ERROR_COLOR));
    if (ui->temperature_label == NULL || ui->humidity_label == NULL || ui->pressure_label == NULL ||
        ui->gas_label == NULL || ui->air_label == NULL || ui->network_label == NULL) goto no_mem;
    ui->air_status_dot = lcd_ui_status_dot(ui->dashboard, 39, 185, lv_color_hex(LCD_UI_INIT_COLOR));
    ui->network_status_dot = lcd_ui_status_dot(ui->dashboard, 82, 258, lv_color_hex(LCD_UI_ERROR_COLOR));
    if (ui->air_status_dot == NULL || ui->network_status_dot == NULL) goto no_mem;

    ui->cat = lv_image_create(ui->dashboard);
    if (ui->cat == NULL) goto no_mem;
    lv_image_set_src(ui->cat, &lcd_asset_cat_idle);
    lv_obj_set_pos(ui->cat, 156, 180);
    ui->cat_hit_area = lv_obj_create(ui->dashboard);
    if (ui->cat_hit_area == NULL) goto no_mem;
    lv_obj_remove_style_all(ui->cat_hit_area);
    lv_obj_set_size(ui->cat_hit_area, 90, 70);
    lv_obj_set_pos(ui->cat_hit_area, 143, 169);
    lv_obj_clear_flag(ui->cat_hit_area, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(ui->cat_hit_area, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(ui->cat_hit_area, lcd_ui_cat_click, LV_EVENT_CLICKED, ui);
    ui->cat_state_label = lcd_ui_label(ui->dashboard, ui->cat_state_text, 156, 230, 64, lv_color_hex(LCD_UI_TITLE_COLOR));
    if (ui->cat_state_label == NULL) goto no_mem;
    lv_obj_set_style_text_align(ui->cat_state_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    ui->command_overlay = lv_obj_create(screen);
    if (ui->command_overlay == NULL) goto no_mem;
    lv_obj_set_pos(ui->command_overlay, 14, 108);
    lv_obj_set_size(ui->command_overlay, 212, 86);
    lv_obj_set_style_radius(ui->command_overlay, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ui->command_overlay, lv_color_hex(LCD_UI_BG_COLOR), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui->command_overlay, LV_OPA_90, LV_PART_MAIN);
    ui->command_label = lcd_ui_label(ui->command_overlay, ui->command_text, 10, 10, 192, lv_color_hex(0xFFFFFF));
    if (ui->command_label == NULL) goto no_mem;
    lv_label_set_long_mode(ui->command_label, LV_LABEL_LONG_WRAP);
    lv_obj_add_flag(ui->command_overlay, LV_OBJ_FLAG_HIDDEN);

    ui->voice_overlay = lv_obj_create(screen);
    if (ui->voice_overlay == NULL) goto no_mem;
    lv_obj_remove_style_all(ui->voice_overlay);
    lv_obj_set_size(ui->voice_overlay, LCD_BOARD_HRES, LCD_BOARD_VRES);
    lv_obj_set_style_bg_color(ui->voice_overlay, lv_color_hex(LCD_UI_BG_COLOR), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui->voice_overlay, LV_OPA_COVER, LV_PART_MAIN);
    ui->voice_cat = lv_image_create(ui->voice_overlay);
    if (ui->voice_cat == NULL) goto no_mem;
    lv_image_set_src(ui->voice_cat, &lcd_asset_cat_idle_voice);
    lv_obj_set_pos(ui->voice_cat, 56, 94);
    lv_obj_add_flag(ui->voice_overlay, LV_OBJ_FLAG_HIDDEN);

    ui->boot_page = lv_obj_create(screen);
    if (ui->boot_page == NULL) goto no_mem;
    lv_obj_remove_style_all(ui->boot_page);
    lv_obj_set_size(ui->boot_page, LCD_BOARD_HRES, LCD_BOARD_VRES);
    lv_obj_set_style_bg_color(ui->boot_page, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui->boot_page, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_t *boot_cat = lv_image_create(ui->boot_page);
    if (boot_cat == NULL) goto no_mem;
    lv_image_set_src(boot_cat, &lcd_asset_cat_boot);
    lv_obj_set_pos(boot_cat, 72, 48);
    ui->boot_display_label = lcd_ui_label(ui->boot_page, ui->boot_display_text, 68, 125, 140, lv_color_hex(LCD_UI_INIT_COLOR));
    ui->boot_sensor_label = lcd_ui_label(ui->boot_page, ui->boot_sensor_text, 68, 150, 140, lv_color_hex(LCD_UI_INIT_COLOR));
    ui->boot_network_label = lcd_ui_label(ui->boot_page, ui->boot_network_text, 68, 175, 140, lv_color_hex(LCD_UI_INIT_COLOR));
    ui->boot_audio_label = lcd_ui_label(ui->boot_page, ui->boot_audio_text, 68, 200, 140, lv_color_hex(LCD_UI_INIT_COLOR));
    ui->boot_display_dot = lcd_ui_status_dot(ui->boot_page, 50, 125, lv_color_hex(LCD_UI_INIT_COLOR));
    ui->boot_sensor_dot = lcd_ui_status_dot(ui->boot_page, 50, 150, lv_color_hex(LCD_UI_INIT_COLOR));
    ui->boot_network_dot = lcd_ui_status_dot(ui->boot_page, 50, 175, lv_color_hex(LCD_UI_INIT_COLOR));
    ui->boot_audio_dot = lcd_ui_status_dot(ui->boot_page, 50, 200, lv_color_hex(LCD_UI_INIT_COLOR));
    ui->boot_progress = lv_bar_create(ui->boot_page);
    if (ui->boot_display_label == NULL || ui->boot_sensor_label == NULL || ui->boot_network_label == NULL ||
        ui->boot_audio_label == NULL || ui->boot_display_dot == NULL || ui->boot_sensor_dot == NULL ||
        ui->boot_network_dot == NULL || ui->boot_audio_dot == NULL || ui->boot_progress == NULL) goto no_mem;
    lv_obj_set_pos(ui->boot_progress, 40, 240);
    lv_obj_set_size(ui->boot_progress, 160, 8);
    lv_bar_set_range(ui->boot_progress, 0, 100);
    lv_bar_set_value(ui->boot_progress, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(ui->boot_progress, lv_color_hex(LCD_UI_BG_COLOR), LV_PART_MAIN);
    lv_obj_set_style_bg_color(ui->boot_progress, lv_color_hex(LCD_UI_ACCENT_COLOR), LV_PART_INDICATOR);
    ui->boot_started_tick = lv_tick_get();
    ui->boot_timer = lv_timer_create(lcd_ui_boot_timer, LCD_UI_BOOT_CHECK_MS, ui);
    ui->animation_timer = lv_timer_create(lcd_ui_animation_timer, LCD_UI_ANIMATION_MS, ui);
    if (ui->boot_timer == NULL || ui->animation_timer == NULL) goto no_mem;
    lv_timer_set_repeat_count(ui->boot_timer, -1);
    lv_timer_set_auto_delete(ui->boot_timer, false);
    return lcd_ui_create_touch(ui);

no_mem:
    return ESP_ERR_NO_MEM;
}

esp_err_t lcd_ui_prepare_lvgl_pool(void *user_ctx)
{
    (void)user_ctx;
    if (s_ui != NULL) {
        return ESP_OK;
    }
    if (lcd_fault_injection_should_fail(LCD_FAULT_UI_ARENA)) {
        return ESP_ERR_NO_MEM;
    }
    const uint32_t caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    if (heap_caps_get_free_size(caps) < LCD_UI_ARENA_BYTES ||
        heap_caps_get_largest_free_block(caps) < LCD_UI_ARENA_BYTES) {
        ESP_LOGE(TAG, "LCD_UI_PSRAM_ADMISSION_FAIL free=%u largest=%u required=%u",
                 (unsigned)heap_caps_get_free_size(caps),
                 (unsigned)heap_caps_get_largest_free_block(caps),
                 (unsigned)LCD_UI_ARENA_BYTES);
        return ESP_ERR_NO_MEM;
    }
    lcd_ui_context_t *ui = heap_caps_calloc(1, LCD_UI_ARENA_BYTES, caps);
    if (ui == NULL) {
        return ESP_ERR_NO_MEM;
    }
    ui->arena = (uint8_t *)ui;
    void *const pool_addr = ui->arena + LCD_UI_POOL_OFFSET_BYTES;
    const size_t pool_size = LCD_UI_POOL_BYTES;
    ESP_LOGI(TAG,
             "LVGL_MEM_INIT_STAGE before_pool addr=%p size=%u align=%u builtin=%u expand=%u psram_free=%u psram_largest=%u",
             pool_addr,
             (unsigned)pool_size,
             (unsigned)((uintptr_t)pool_addr % sizeof(void *)),
             (unsigned)LV_MEM_SIZE,
             (unsigned)LV_MEM_POOL_EXPAND_SIZE,
             (unsigned)heap_caps_get_free_size(caps),
             (unsigned)heap_caps_get_largest_free_block(caps));
    ui->lvgl_pool = lv_mem_add_pool(pool_addr, pool_size);
    ESP_LOGI(TAG,
             "LVGL_MEM_INIT_STAGE after_pool pool=%p psram_free=%u psram_largest=%u",
             ui->lvgl_pool,
             (unsigned)heap_caps_get_free_size(caps),
             (unsigned)heap_caps_get_largest_free_block(caps));
    if (ui->lvgl_pool == NULL) {
        heap_caps_free(ui);
        ESP_LOGE(TAG, "LCD_UI LVGL PSRAM pool registration failed");
        return ESP_ERR_NO_MEM;
    }
    s_ui = ui;
    ESP_LOGI(TAG, "LCD_UI_POOL_READY psram_arena=%u", (unsigned)LCD_UI_ARENA_BYTES);
    return ESP_OK;
}

void lcd_ui_release_lvgl_pool(void *user_ctx)
{
    (void)user_ctx;
    lcd_ui_context_t *const ui = s_ui;
    if (ui == NULL) {
        return;
    }
    s_ui = NULL;
    /* lv_deinit() has already released LVGL's pool metadata and theme data. */
    heap_caps_free(ui);
}

esp_err_t lcd_ui_start(lv_display_t *display, lcd_ui_wake_request_fn wake_request, void *wake_ctx)
{
    if (display == NULL || s_ui == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_ui->display != NULL) {
        return s_ui->display == display ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    lcd_ui_context_t *const ui = s_ui;
    ui->display = display;
    ui->wake_request = wake_request;
    ui->wake_ctx = wake_ctx;
    (void)snprintf(ui->temperature_text, sizeof(ui->temperature_text), "TEMP   --");
    (void)snprintf(ui->humidity_text, sizeof(ui->humidity_text), "HUM    --");
    (void)snprintf(ui->pressure_text, sizeof(ui->pressure_text), "PRESS  --");
    (void)snprintf(ui->gas_text, sizeof(ui->gas_text), "GAS    --");
    (void)snprintf(ui->air_text, sizeof(ui->air_text), "AIR    --");
    (void)snprintf(ui->network_text, sizeof(ui->network_text), "Offline");
    (void)snprintf(ui->cat_state_text, sizeof(ui->cat_state_text), "IDLE");
    (void)snprintf(ui->boot_display_text, sizeof(ui->boot_display_text), "Display INIT");
    (void)snprintf(ui->boot_sensor_text, sizeof(ui->boot_sensor_text), "Sensor INIT");
    (void)snprintf(ui->boot_network_text, sizeof(ui->boot_network_text), "Network INIT");
    (void)snprintf(ui->boot_audio_text, sizeof(ui->boot_audio_text), "Audio INIT");
    ui->command_text[0] = '\0';
    const esp_err_t ret = lcd_ui_create(ui);
    if (ret != ESP_OK) {
        (void)lcd_ui_stop();
        return ret;
    }
    ESP_LOGI(TAG, "LCD_UI_READY psram_arena=%u", (unsigned)LCD_UI_ARENA_BYTES);
    return ESP_OK;
}

esp_err_t lcd_ui_stop(void)
{
    lcd_ui_context_t *const ui = s_ui;
    if (ui == NULL) {
        return ESP_OK;
    }
    if (ui->animation_timer != NULL) {
        lv_timer_delete(ui->animation_timer);
        ui->animation_timer = NULL;
    }
    if (ui->boot_timer != NULL) {
        lv_timer_delete(ui->boot_timer);
        ui->boot_timer = NULL;
    }
    if (ui->touch_indev != NULL) {
        lv_indev_delete(ui->touch_indev);
        ui->touch_indev = NULL;
    }
    (void)lcd_touch_stop();
    if (ui->boot_page != NULL) {
        lv_obj_delete(ui->boot_page);
        ui->boot_page = NULL;
    }
    if (ui->command_overlay != NULL) {
        lv_obj_delete(ui->command_overlay);
        ui->command_overlay = NULL;
    }
    if (ui->voice_overlay != NULL) {
        lv_obj_delete(ui->voice_overlay);
        ui->voice_overlay = NULL;
        ui->voice_cat = NULL;
    }
    if (ui->dashboard != NULL) {
        lv_obj_delete(ui->dashboard);
        ui->dashboard = NULL;
    }
    return ESP_OK;
}

void lcd_ui_apply(const lcd_system_snapshot_t *snapshot,
                  const lcd_command_t *command,
                  bool command_visible,
                  bool full_refresh,
                  bool boot_complete)
{
    lcd_ui_context_t *const ui = s_ui;
    if (ui == NULL || snapshot == NULL) {
        return;
    }
    ui->snapshot = *snapshot;
    ui->boot_complete = boot_complete;
    if (full_refresh) {
        const bool temperature_valid = snapshot->bme_valid && isfinite(snapshot->temperature_c);
        const bool humidity_valid = snapshot->bme_valid && isfinite(snapshot->humidity_percent) &&
                                    snapshot->humidity_percent >= 0.0f && snapshot->humidity_percent <= 100.0f;
        const bool pressure_valid = snapshot->bme_valid && isfinite(snapshot->pressure_hpa) &&
                                    snapshot->pressure_hpa > 0.0f;
        const bool gas_valid = snapshot->bme_valid && snapshot->gas_valid && snapshot->gas_resistance_ohm != 0U;
        if (temperature_valid) {
            (void)snprintf(ui->temperature_text, sizeof(ui->temperature_text), "TEMP   %.1f C",
                           (double)snapshot->temperature_c);
        } else {
            (void)snprintf(ui->temperature_text, sizeof(ui->temperature_text), "TEMP   --");
        }
        if (humidity_valid) {
            (void)snprintf(ui->humidity_text, sizeof(ui->humidity_text), "HUM    %.1f %%",
                           (double)snapshot->humidity_percent);
        } else {
            (void)snprintf(ui->humidity_text, sizeof(ui->humidity_text), "HUM    --");
        }
        if (pressure_valid) {
            (void)snprintf(ui->pressure_text, sizeof(ui->pressure_text), "PRESS  %.1f hPa",
                           (double)snapshot->pressure_hpa);
        } else {
            (void)snprintf(ui->pressure_text, sizeof(ui->pressure_text), "PRESS  --");
        }
        if (gas_valid) {
            (void)snprintf(ui->gas_text, sizeof(ui->gas_text), "GAS    %.1f kOhm",
                           (double)snapshot->gas_resistance_ohm / 1000.0);
        } else {
            (void)snprintf(ui->gas_text, sizeof(ui->gas_text), "GAS    --");
        }
        const bool air_valid = snapshot->bme_valid && snapshot->air_quality_valid;
        (void)snprintf(ui->air_text, sizeof(ui->air_text), air_valid ? "AIR    IAQ %u" : "AIR    --",
                       (unsigned)snapshot->iaq);
        const char *network = snapshot->wifi_state == LCD_WIFI_ONLINE && snapshot->gateway_online ? "Online" :
                              snapshot->wifi_state == LCD_WIFI_CONNECTING || snapshot->wifi_state == LCD_WIFI_ONLINE ?
                              "Connecting" : "Offline";
        (void)snprintf(ui->network_text, sizeof(ui->network_text), "%s", network);
        (void)snprintf(ui->cat_state_text, sizeof(ui->cat_state_text), "%s", lcd_ui_voice_name(snapshot->voice_state));
        const lv_color_t air_color = lv_color_hex(air_valid ? LCD_UI_ACCENT_COLOR : LCD_UI_INIT_COLOR);
        const lv_color_t network_color = lcd_ui_network_color(snapshot);
        lcd_ui_set_text(ui->temperature_label, ui->temperature_text);
        lcd_ui_set_text(ui->humidity_label, ui->humidity_text);
        lcd_ui_set_text(ui->pressure_label, ui->pressure_text);
        lcd_ui_set_text(ui->gas_label, ui->gas_text);
        lcd_ui_set_text(ui->air_label, ui->air_text);
        lcd_ui_set_text(ui->network_label, ui->network_text);
        lcd_ui_set_text(ui->cat_state_label, ui->cat_state_text);
        lv_obj_set_style_text_color(ui->air_label, air_color, LV_PART_MAIN);
        lv_obj_set_style_bg_color(ui->air_status_dot, air_color, LV_PART_MAIN);
        lv_obj_set_style_text_color(ui->network_label, network_color, LV_PART_MAIN);
        lv_obj_set_style_bg_color(ui->network_status_dot, network_color, LV_PART_MAIN);
    }
    if (ui->voice_overlay != NULL && ui->dashboard != NULL) {
        const bool active = snapshot->voice_state != LCD_VOICE_IDLE;
        if (active) {
            lv_obj_add_flag(ui->dashboard, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(ui->voice_overlay, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(ui->voice_overlay, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(ui->dashboard, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (ui->command_overlay != NULL) {
        if (command_visible && command != NULL) {
            (void)snprintf(ui->command_text, sizeof(ui->command_text), "%s\n%s", command->title, command->text);
            lcd_ui_set_text(ui->command_label, ui->command_text);
            lv_obj_clear_flag(ui->command_overlay, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(ui->command_overlay, LV_OBJ_FLAG_HIDDEN);
        }
    }
}
