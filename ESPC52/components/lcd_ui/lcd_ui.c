#include "lcd_ui.h"

#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "lcd_assets.h"
#include "lcd_board_profile.h"
#include "lcd_touch.h"

#define LCD_UI_ARENA_BYTES (96U * 1024U)
#define LCD_UI_ANIMATION_MS 100U
#define LCD_UI_BOOT_MS 3000U

static const char *TAG = "lcd_ui";

typedef struct {
    uint8_t *arena;
    lv_mem_pool_t lvgl_pool;
    lv_display_t *display;
    lv_indev_t *touch_indev;
    lv_timer_t *boot_timer;
    lv_timer_t *animation_timer;
    lv_obj_t *dashboard;
    lv_obj_t *status_page;
    lv_obj_t *boot_page;
    lv_obj_t *command_overlay;
    lv_obj_t *cat;
    lv_obj_t *wifi_label;
    lv_obj_t *air_label;
    lv_obj_t *radar_label;
    lv_obj_t *home_label;
    lv_obj_t *voice_label;
    lv_obj_t *alarm_label;
    lv_obj_t *command_label;
    lcd_ui_wake_request_fn wake_request;
    void *wake_ctx;
    lcd_system_snapshot_t snapshot;
    uint32_t animation_frame;
    char wifi_text[32];
    char air_text[48];
    char radar_text[72];
    char home_text[56];
    char voice_text[40];
    char alarm_text[40];
    char command_text[LCD_COMMAND_TITLE_MAX + LCD_COMMAND_TEXT_MAX + 8U];
} lcd_ui_context_t;

static lcd_ui_context_t *s_ui;

_Static_assert(sizeof(lcd_ui_context_t) <= 4096U, "UI control block must leave the PSRAM arena intact");

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

static lv_obj_t *lcd_ui_card(lv_obj_t *parent, int16_t x, int16_t y, int16_t width, int16_t height)
{
    lv_obj_t *card = lv_obj_create(parent);
    if (card == NULL) {
        return NULL;
    }
    lv_obj_set_pos(card, x, y);
    lv_obj_set_size(card, width, height);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(card, 6, LV_PART_MAIN);
    lv_obj_set_style_border_width(card, 1, LV_PART_MAIN);
    lv_obj_set_style_border_color(card, lv_color_hex(0x2B3F4E), LV_PART_MAIN);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x172531), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(card, 0, LV_PART_MAIN);
    return card;
}

static void lcd_ui_set_text(lv_obj_t *label, const char *text)
{
    if (label != NULL && text != NULL) {
        lv_label_set_text_static(label, text);
        lv_obj_invalidate(label);
    }
}

static const char *lcd_ui_wifi_name(lcd_wifi_state_t state)
{
    switch (state) {
    case LCD_WIFI_ONLINE: return "ONLINE";
    case LCD_WIFI_CONNECTING: return "CONNECTING";
    case LCD_WIFI_OFFLINE: return "OFFLINE";
    case LCD_WIFI_UNKNOWN:
    default: return "UNKNOWN";
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

static void lcd_ui_status_click(lv_event_t *event)
{
    lcd_ui_context_t *const ui = lv_event_get_user_data(event);
    if (ui == NULL || ui->status_page == NULL) {
        return;
    }
    if (lv_obj_has_flag(ui->status_page, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_clear_flag(ui->status_page, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(ui->status_page, LV_OBJ_FLAG_HIDDEN);
    }
}

static void lcd_ui_boot_timer(lv_timer_t *timer)
{
    lcd_ui_context_t *const ui = timer != NULL ? lv_timer_get_user_data(timer) : NULL;
    if (ui != NULL && ui->boot_page != NULL) {
        lv_obj_add_flag(ui->boot_page, LV_OBJ_FLAG_HIDDEN);
    }
    if (timer != NULL) {
        lv_timer_delete(timer);
    }
    if (ui != NULL) {
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
    const bool active = ui->snapshot.speaker_active || ui->snapshot.voice_state == LCD_VOICE_RECORDING ||
                        ui->snapshot.voice_state == LCD_VOICE_PLAYING;
    lv_image_set_src(ui->cat, active && (ui->animation_frame & 1U) ? &lcd_asset_cat_talk : &lcd_asset_cat_idle);
    lv_obj_set_y(ui->cat, (int32_t)(184 + ((ui->animation_frame & 1U) ? -1 : 0)));
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
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x101820), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);

    ui->dashboard = lv_obj_create(screen);
    if (ui->dashboard == NULL) goto no_mem;
    lv_obj_remove_style_all(ui->dashboard);
    lv_obj_set_size(ui->dashboard, LCD_BOARD_HRES, LCD_BOARD_VRES);
    lv_obj_clear_flag(ui->dashboard, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(ui->dashboard, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(ui->dashboard, lcd_ui_status_click, LV_EVENT_CLICKED, ui);

    if (lcd_ui_label(ui->dashboard, "SENSAIR C5", 12, 10, 140, lv_color_hex(0xDCE7EF)) == NULL) goto no_mem;
    ui->wifi_label = lcd_ui_label(ui->dashboard, ui->wifi_text, 152, 10, 80, lv_color_hex(0x79D2A6));
    if (ui->wifi_label == NULL) goto no_mem;

    lv_obj_t *card = lcd_ui_card(ui->dashboard, 10, 38, 220, 48);
    if (card == NULL) goto no_mem;
    if (lcd_ui_label(card, "AIR", 10, 7, 36, lv_color_hex(0x8EA8B7)) == NULL) goto no_mem;
    ui->air_label = lcd_ui_label(card, ui->air_text, 50, 7, 160, lv_color_hex(0xDCE7EF));
    if (ui->air_label == NULL) goto no_mem;

    card = lcd_ui_card(ui->dashboard, 10, 94, 220, 70);
    if (card == NULL) goto no_mem;
    if (lcd_ui_label(card, "SPACE", 10, 7, 52, lv_color_hex(0x8EA8B7)) == NULL) goto no_mem;
    ui->radar_label = lcd_ui_label(card, ui->radar_text, 10, 27, 200, lv_color_hex(0x79D2A6));
    if (ui->radar_label == NULL) goto no_mem;

    card = lcd_ui_card(ui->dashboard, 10, 171, 126, 70);
    if (card == NULL) goto no_mem;
    ui->home_label = lcd_ui_label(card, ui->home_text, 10, 8, 110, lv_color_hex(0xDCE7EF));
    if (ui->home_label == NULL) goto no_mem;

    ui->cat = lv_image_create(ui->dashboard);
    if (ui->cat == NULL) goto no_mem;
    lv_image_set_src(ui->cat, &lcd_asset_cat_idle);
    lv_obj_set_pos(ui->cat, 180, 184);
    lv_obj_add_flag(ui->cat, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(ui->cat, lcd_ui_cat_click, LV_EVENT_CLICKED, ui);

    card = lcd_ui_card(ui->dashboard, 10, 248, 220, 28);
    if (card == NULL) goto no_mem;
    ui->voice_label = lcd_ui_label(card, ui->voice_text, 8, 5, 100, lv_color_hex(0xDCE7EF));
    ui->alarm_label = lcd_ui_label(card, ui->alarm_text, 112, 5, 100, lv_color_hex(0xE5B75B));
    if (ui->voice_label == NULL || ui->alarm_label == NULL) goto no_mem;

    ui->status_page = lv_obj_create(screen);
    if (ui->status_page == NULL) goto no_mem;
    lv_obj_remove_style_all(ui->status_page);
    lv_obj_set_size(ui->status_page, LCD_BOARD_HRES, LCD_BOARD_VRES);
    lv_obj_set_style_bg_color(ui->status_page, lv_color_hex(0x101820), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui->status_page, LV_OPA_COVER, LV_PART_MAIN);
    if (lcd_ui_label(ui->status_page, "SYSTEM STATUS", 18, 22, 200, lv_color_hex(0xDCE7EF)) == NULL ||
        lcd_ui_label(ui->status_page, "Wi-Fi, BME, radar, voice and gateway", 18, 64, 204, lv_color_hex(0x8EA8B7)) == NULL ||
        lcd_ui_label(ui->status_page, "Touch anywhere to return", 18, 238, 204, lv_color_hex(0x79D2A6)) == NULL) goto no_mem;
    lv_obj_add_flag(ui->status_page, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(ui->status_page, lcd_ui_status_click, LV_EVENT_CLICKED, ui);
    lv_obj_add_flag(ui->status_page, LV_OBJ_FLAG_HIDDEN);

    ui->command_overlay = lv_obj_create(screen);
    if (ui->command_overlay == NULL) goto no_mem;
    lv_obj_set_pos(ui->command_overlay, 14, 108);
    lv_obj_set_size(ui->command_overlay, 212, 86);
    lv_obj_set_style_radius(ui->command_overlay, 6, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ui->command_overlay, lv_color_hex(0x263B4A), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui->command_overlay, LV_OPA_90, LV_PART_MAIN);
    ui->command_label = lcd_ui_label(ui->command_overlay, ui->command_text, 10, 10, 192, lv_color_hex(0xFFFFFF));
    if (ui->command_label == NULL) goto no_mem;
    lv_label_set_long_mode(ui->command_label, LV_LABEL_LONG_WRAP);
    lv_obj_add_flag(ui->command_overlay, LV_OBJ_FLAG_HIDDEN);

    ui->boot_page = lv_obj_create(screen);
    if (ui->boot_page == NULL) goto no_mem;
    lv_obj_remove_style_all(ui->boot_page);
    lv_obj_set_size(ui->boot_page, LCD_BOARD_HRES, LCD_BOARD_VRES);
    lv_obj_set_style_bg_color(ui->boot_page, lv_color_hex(0x101820), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ui->boot_page, LV_OPA_COVER, LV_PART_MAIN);
    if (lcd_ui_label(ui->boot_page, "SENSAIR", 0, 36, LCD_BOARD_HRES, lv_color_hex(0xDCE7EF)) == NULL ||
        lcd_ui_label(ui->boot_page, "RADAR HOME", 0, 62, LCD_BOARD_HRES, lv_color_hex(0x79D2A6)) == NULL ||
        lcd_ui_label(ui->boot_page, "Display  Sensor  Network  Audio", 22, 208, 200, lv_color_hex(0xE5B75B)) == NULL) goto no_mem;
    lv_obj_t *boot_cat = lv_image_create(ui->boot_page);
    if (boot_cat == NULL) goto no_mem;
    lv_image_set_src(boot_cat, &lcd_asset_cat_idle);
    lv_obj_set_pos(boot_cat, 104, 112);

    ui->boot_timer = lv_timer_create(lcd_ui_boot_timer, LCD_UI_BOOT_MS, ui);
    ui->animation_timer = lv_timer_create(lcd_ui_animation_timer, LCD_UI_ANIMATION_MS, ui);
    if (ui->boot_timer == NULL || ui->animation_timer == NULL) goto no_mem;
    lv_timer_set_repeat_count(ui->boot_timer, 1);
    lv_timer_set_auto_delete(ui->boot_timer, false);
    return lcd_ui_create_touch(ui);

no_mem:
    return ESP_ERR_NO_MEM;
}

esp_err_t lcd_ui_start(lv_display_t *display, lcd_ui_wake_request_fn wake_request, void *wake_ctx)
{
    if (s_ui != NULL) {
        return ESP_OK;
    }
    if (display == NULL) {
        return ESP_ERR_INVALID_ARG;
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
    ui->display = display;
    ui->wake_request = wake_request;
    ui->wake_ctx = wake_ctx;
    (void)snprintf(ui->wifi_text, sizeof(ui->wifi_text), "UNKNOWN");
    (void)snprintf(ui->air_text, sizeof(ui->air_text), "BME WAITING");
    (void)snprintf(ui->radar_text, sizeof(ui->radar_text), "RADAR WAITING");
    (void)snprintf(ui->home_text, sizeof(ui->home_text), "HOME\nWAITING");
    (void)snprintf(ui->voice_text, sizeof(ui->voice_text), "VOICE IDLE");
    (void)snprintf(ui->alarm_text, sizeof(ui->alarm_text), "ALARM 0");
    ui->command_text[0] = '\0';

    ui->lvgl_pool = lv_mem_add_pool(ui->arena + 4096U, LCD_UI_ARENA_BYTES - 4096U);
    if (ui->lvgl_pool == NULL) {
        heap_caps_free(ui);
        ESP_LOGE(TAG, "LCD_UI LVGL PSRAM pool registration failed");
        return ESP_ERR_NO_MEM;
    }
    s_ui = ui;
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
    s_ui = NULL;
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
    }
    if (ui->command_overlay != NULL) {
        lv_obj_delete(ui->command_overlay);
    }
    if (ui->status_page != NULL) {
        lv_obj_delete(ui->status_page);
    }
    if (ui->dashboard != NULL) {
        lv_obj_delete(ui->dashboard);
    }
    if (ui->lvgl_pool != NULL) {
        lv_mem_remove_pool(ui->lvgl_pool);
        ui->lvgl_pool = NULL;
    }
    heap_caps_free(ui);
    return ESP_OK;
}

void lcd_ui_apply(const lcd_system_snapshot_t *snapshot,
                  const lcd_command_t *command,
                  bool command_visible,
                  bool full_refresh)
{
    lcd_ui_context_t *const ui = s_ui;
    if (ui == NULL || snapshot == NULL) {
        return;
    }
    ui->snapshot = *snapshot;
    if (full_refresh) {
        uint8_t healthy_sources = 0U;
        uint8_t people = 0U;
        bool motion = false;
        for (size_t i = 0; i < LCD_SYSTEM_SNAPSHOT_RADAR_SOURCES; ++i) {
            healthy_sources += snapshot->radar_sources[i].healthy ? 1U : 0U;
            people += snapshot->radar_sources[i].person_count;
            motion = motion || snapshot->radar_sources[i].motion;
        }
        (void)snprintf(ui->wifi_text, sizeof(ui->wifi_text), "%s %s",
                       lcd_ui_wifi_name(snapshot->wifi_state), snapshot->gateway_online ? "GW" : "");
        if (snapshot->bme_valid) {
            (void)snprintf(ui->air_text, sizeof(ui->air_text), "%.1f C  %.0f %%  IAQ %u",
                           (double)snapshot->temperature_c,
                           (double)snapshot->humidity_percent,
                           (unsigned)snapshot->iaq);
        } else {
            (void)snprintf(ui->air_text, sizeof(ui->air_text), "BME WAITING");
        }
        (void)snprintf(ui->radar_text, sizeof(ui->radar_text), "%u/3 SOURCES  %u PEOPLE  %s",
                       (unsigned)healthy_sources, (unsigned)people, motion ? "MOTION" : "STILL");
        if ((snapshot->degraded_flags & (1U << 0)) != 0U) {
            (void)snprintf(ui->home_text, sizeof(ui->home_text), "ROOM %s\nHOME REMOTE",
                           snapshot->room_occupied ? "OCCUPIED" : "EMPTY");
        } else {
            (void)snprintf(ui->home_text, sizeof(ui->home_text), "ROOM %s\nHOME %u %s",
                           snapshot->room_occupied ? "OCCUPIED" : "EMPTY",
                           (unsigned)snapshot->home_person_count,
                           snapshot->home_occupied ? "PRESENT" : "EMPTY");
        }
        (void)snprintf(ui->voice_text, sizeof(ui->voice_text), "VOICE %s", lcd_ui_voice_name(snapshot->voice_state));
        (void)snprintf(ui->alarm_text, sizeof(ui->alarm_text), "ALARM %u", (unsigned)snapshot->alarm_level);
        lcd_ui_set_text(ui->wifi_label, ui->wifi_text);
        lcd_ui_set_text(ui->air_label, ui->air_text);
        lcd_ui_set_text(ui->radar_label, ui->radar_text);
        lcd_ui_set_text(ui->home_label, ui->home_text);
        lcd_ui_set_text(ui->voice_label, ui->voice_text);
        lcd_ui_set_text(ui->alarm_label, ui->alarm_text);
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
