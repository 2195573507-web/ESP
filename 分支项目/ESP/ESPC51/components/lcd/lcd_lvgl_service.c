#include <stdio.h>

#include "lcd_service.h"

#include "lcd.h"
#include "touch_cst816t.h"
#include "boot_screen.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "lvgl.h"

extern esp_err_t voice_chain_request_local_wake(void);
extern void boot_screen_set_dashboard_refresh_timer(lv_timer_t *timer);

#define LCD_LVGL_DRAW_BUFFER_LINES 1U
#define LCD_LVGL_DRAW_BUFFER_PIXELS (LCD_H_RES * LCD_LVGL_DRAW_BUFFER_LINES)
#define LCD_LVGL_TASK_PRIORITY 1
#define LCD_LVGL_TASK_STACK 4096
#define LCD_LVGL_TIMER_PERIOD_MS 20
#define LCD_LVGL_MEM_LOG_PERIOD_MS 30000U
#define LCD_DASHBOARD_SNAPSHOT_REFRESH_MS 100U
#define LCD_DASHBOARD_DATA_REFRESH_MS 1000LL
#define LCD_DASHBOARD_LOG_PERIOD_MS 10000LL
#define LCD_DASHBOARD_MARGIN_X 12
#define LCD_DASHBOARD_TEXT_WIDTH (LCD_H_RES - (LCD_DASHBOARD_MARGIN_X * 2))
#define LCD_DASHBOARD_ENV_COLUMN_WIDTH 144
#define LCD_DASHBOARD_STATUS_DOT_SIZE 6
#define LCD_DASHBOARD_STATUS_DOT_X 39
#define LCD_DASHBOARD_AIR_DOT_Y 185
#define LCD_DASHBOARD_CONNECTION_DOT_X 82
#define LCD_DASHBOARD_CONNECTION_DOT_Y 258
#define LCD_DASHBOARD_CONNECTION_LABEL_X 98
#define LCD_DASHBOARD_CONNECTION_LABEL_Y 253
#define LCD_DASHBOARD_CONNECTION_LABEL_WIDTH 100
#define LCD_TOUCH_MIRROR_Y_ENABLE 1
#define LCD_CAT_ANIMATION_PERIOD_MS 100U
#define LCD_CAT_WAKE_FRAMES 6U
#define LCD_CAT_RECORDING_FRAMES 6U
#define LCD_CAT_PLAY_MOUTH_PERIOD_FRAMES 4U
#define LCD_CAT_IMAGE_WIDTH 64U
#define LCD_CAT_IMAGE_HEIGHT 48U
#define LCD_CAT_IMAGE_STRIDE 32U
#define LCD_CAT_IMAGE_PALETTE_BYTES 64U
#define LCD_CAT_IMAGE_DATA_BYTES (LCD_CAT_IMAGE_PALETTE_BYTES + \
                                  (LCD_CAT_IMAGE_STRIDE * LCD_CAT_IMAGE_HEIGHT))
#define LCD_CAT_VOICE_SCALE 2U
#define LCD_CAT_VOICE_IMAGE_WIDTH (LCD_CAT_IMAGE_WIDTH * LCD_CAT_VOICE_SCALE)
#define LCD_CAT_VOICE_IMAGE_HEIGHT (LCD_CAT_IMAGE_HEIGHT * LCD_CAT_VOICE_SCALE)
#define LCD_CAT_VOICE_IMAGE_STRIDE (LCD_CAT_IMAGE_STRIDE * LCD_CAT_VOICE_SCALE)
#define LCD_CAT_VOICE_IMAGE_DATA_BYTES (LCD_CAT_IMAGE_PALETTE_BYTES + \
                                        (LCD_CAT_VOICE_IMAGE_STRIDE * LCD_CAT_VOICE_IMAGE_HEIGHT))
#define LCD_CAT_IMAGE_X (LCD_H_RES - LCD_DASHBOARD_MARGIN_X - LCD_CAT_IMAGE_WIDTH - 8)
#define LCD_CAT_IMAGE_Y 180
#define LCD_DASHBOARD_CAT_HIT_WIDTH 90U
#define LCD_DASHBOARD_CAT_HIT_HEIGHT 70U
#define LCD_DASHBOARD_CAT_HIT_X \
    (LCD_CAT_IMAGE_X - ((LCD_DASHBOARD_CAT_HIT_WIDTH - LCD_CAT_IMAGE_WIDTH) / 2))
#define LCD_DASHBOARD_CAT_HIT_Y \
    (LCD_CAT_IMAGE_Y - ((LCD_DASHBOARD_CAT_HIT_HEIGHT - LCD_CAT_IMAGE_HEIGHT) / 2))
#define LCD_VOICE_CAT_IMAGE_X ((LCD_H_RES - LCD_CAT_VOICE_IMAGE_WIDTH) / 2)
#define LCD_VOICE_CAT_IMAGE_Y ((LCD_V_RES - LCD_CAT_VOICE_IMAGE_HEIGHT) / 2)

static const char *TAG = "LCD_LVGL";
static const char *MEM_TAG = "LVGL_MEM";
static lv_display_t *s_display;
static lv_indev_t *s_touch_indev;
static bool s_started;
static lv_obj_t *s_temp_label;
static lv_obj_t *s_humidity_label;
static lv_obj_t *s_pressure_label;
static lv_obj_t *s_gas_label;
static lv_obj_t *s_air_label;
static lv_obj_t *s_network_label;
static lv_obj_t *s_voice_label;
static lv_obj_t *s_air_status_dot;
static lv_obj_t *s_connection_status_dot;
static lv_obj_t *s_dashboard_root;
static lv_obj_t *s_voice_root;
static lv_obj_t *s_cat_image;
static lv_obj_t *s_cat_hit_area;
static lv_timer_t *s_cat_animation_timer;
static lv_timer_t *s_dashboard_refresh_timer;
static lv_obj_t *s_voice_cat_image;
static lv_timer_t *s_voice_animation_timer;
static char s_temp_text[32];
static char s_humidity_text[32];
static char s_pressure_text[32];
static char s_gas_text[32];
static char s_air_text[32];
static char s_network_text[32];
static char s_voice_text[32];
static lcd_dashboard_snapshot_provider_t s_snapshot_provider;
static void *s_snapshot_provider_ctx;
static int64_t s_last_dashboard_log_ms;
static int64_t s_last_dashboard_data_update_ms;
static portMUX_TYPE s_snapshot_provider_lock = portMUX_INITIALIZER_UNLOCKED;
static lcd_dashboard_voice_state_t s_cat_voice_state;
static uint8_t s_cat_animation_frame;
static uint8_t s_cat_play_frame;
static bool s_cat_voice_state_valid;
static bool s_cat_play_mouth_open;
static bool s_cat_speaker_active;
static bool s_cat_disabled;
static const lv_image_dsc_t *s_cat_frame;
static lcd_dashboard_voice_state_t s_voice_ui_state;
static const lv_image_dsc_t *s_voice_cat_frame;
static uint8_t s_voice_animation_frame;
static uint8_t s_voice_play_frame;
static bool s_voice_play_mouth_open;
static bool s_voice_speaker_active;
static bool s_voice_ui_visible;
static bool s_voice_ui_disabled;
static bool s_touch_pressed;

static void lcd_lvgl_log_memory(const char *stage);
static void lcd_cat_animation_timer_cb(lv_timer_t *timer);
static void lcd_voice_animation_timer_cb(lv_timer_t *timer);

static bool lcd_lvgl_map_touch_point(uint16_t raw_x,
                                     uint16_t raw_y,
                                     int32_t *x,
                                     int32_t *y)
{
    if (x == NULL || y == NULL) {
        return false;
    }

    int32_t mapped_x = raw_x;
    int32_t mapped_y = raw_y;

#if LCD_SWAP_XY_ENABLE
    const int32_t swap = mapped_x;
    mapped_x = mapped_y;
    mapped_y = swap;
#endif
#if LCD_MIRROR_X_ENABLE
    mapped_x = LCD_H_RES - 1 - mapped_x;
#endif
#if LCD_TOUCH_MIRROR_Y_ENABLE
    mapped_y = LCD_V_RES - 1 - mapped_y;
#endif

    if (mapped_x < 0 || mapped_x >= LCD_H_RES || mapped_y < 0 || mapped_y >= LCD_V_RES) {
        return false;
    }

    *x = mapped_x;
    *y = mapped_y;
    return true;
}

static void lcd_lvgl_touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;

    uint16_t raw_x = 0;
    uint16_t raw_y = 0;
    bool pressed = false;
    const esp_err_t ret = cst816t_read_point(&raw_x, &raw_y, &pressed);

    if (ret == ESP_OK && pressed) {
        int32_t x = 0;
        int32_t y = 0;
        if (lcd_lvgl_map_touch_point(raw_x, raw_y, &x, &y)) {
            data->point.x = x;
            data->point.y = y;
            data->state = LV_INDEV_STATE_PRESSED;

            if (!s_touch_pressed) {
                ESP_LOGI(TAG,
                         "LCD_TOUCH_MAP raw=(%d,%d) mapped=(%d,%d)",
                         (int)raw_x,
                         (int)raw_y,
                         (int)x,
                         (int)y);
                ESP_LOGI(TAG, "LCD_TOUCH_CLICK x=%d y=%d", (int)x, (int)y);
            }
            s_touch_pressed = true;
            return;
        }
    }

    data->state = LV_INDEV_STATE_RELEASED;
    s_touch_pressed = false;
}

static esp_err_t lcd_lvgl_register_touch_indev(void)
{
    if (s_touch_indev != NULL) {
        return ESP_OK;
    }
    if (s_display == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!lvgl_port_lock(1000)) {
        return ESP_ERR_TIMEOUT;
    }

    s_touch_indev = lv_indev_create();
    if (s_touch_indev != NULL) {
        lv_indev_set_type(s_touch_indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(s_touch_indev, lcd_lvgl_touch_read_cb);
        lv_indev_set_display(s_touch_indev, s_display);
    }

    lvgl_port_unlock();

    if (s_touch_indev == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "LCD touch pointer indev registered");
    return ESP_OK;
}

static void lcd_dashboard_cat_click_cb(lv_event_t *event)
{
    (void)event;
    esp_err_t ret = voice_chain_request_local_wake();
    if (ret == ESP_OK || ret == ESP_ERR_NOT_FINISHED) {
        ESP_LOGI(TAG, "dashboard cat wake request accepted");
    } else {
        ESP_LOGW(TAG, "dashboard cat wake request rejected: %s", esp_err_to_name(ret));
    }
}

static lv_obj_t *lcd_lvgl_create_label(lv_obj_t *parent,
                                        const char *text,
                                        lv_color_t color,
                                        int16_t y)
{
    lv_obj_t *label = lv_label_create(parent);
    if (label == NULL) {
        return NULL;
    }
    lv_label_set_text_static(label, text);
    lv_obj_set_style_text_color(label, color, LV_PART_MAIN);
    lv_obj_set_width(label, LCD_DASHBOARD_TEXT_WIDTH);
    lv_label_set_long_mode(label, LV_LABEL_LONG_CLIP);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, LCD_DASHBOARD_MARGIN_X, y);
    return label;
}

static lv_obj_t *lcd_lvgl_create_status_dot(lv_obj_t *parent, int16_t x, int16_t y)
{
    lv_obj_t *dot = lv_obj_create(parent);
    if (dot == NULL) {
        return NULL;
    }
    lv_obj_remove_style_all(dot);
    lv_obj_set_pos(dot, x, y);
    lv_obj_set_size(dot, 8, 8);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    return dot;
}

static void lcd_lvgl_set_status_dot_color(lv_obj_t *dot, lv_color_t color)
{
    if (dot != NULL) {
        lv_obj_set_style_bg_color(dot, color, 0);
    }
}

static const char *lcd_dashboard_air_state_name(lcd_dashboard_air_state_t state)
{
    switch (state) {
    case LCD_DASHBOARD_AIR_READY:
        return "GOOD";
    case LCD_DASHBOARD_AIR_DEGRADED:
        return "BAD";
    case LCD_DASHBOARD_AIR_CALIBRATING:
        return "CALIB";
    case LCD_DASHBOARD_AIR_INIT:
    default:
        return "INIT";
    }
}

static lv_color_t lcd_dashboard_air_state_color(lcd_dashboard_air_state_t state)
{
    switch (state) {
    case LCD_DASHBOARD_AIR_READY:
        return lv_color_hex(0x79D2A6);
    case LCD_DASHBOARD_AIR_DEGRADED:
        return lv_color_hex(0xE56B6F);
    case LCD_DASHBOARD_AIR_CALIBRATING:
    case LCD_DASHBOARD_AIR_INIT:
    default:
        return lv_color_hex(0xE5B75B);
    }
}

static const char *lcd_dashboard_voice_state_name(lcd_dashboard_voice_state_t state)
{
    switch (state) {
    case LCD_DASHBOARD_VOICE_WAKE:
        return "WAKE";
    case LCD_DASHBOARD_VOICE_REC:
        return "REC";
    case LCD_DASHBOARD_VOICE_WAIT:
        return "WAIT";
    case LCD_DASHBOARD_VOICE_PLAY:
        return "PLAY";
    case LCD_DASHBOARD_VOICE_ERR:
        return "ERR";
    case LCD_DASHBOARD_VOICE_LISTEN:
    default:
        return "LISTEN";
    }
}

static const char *lcd_dashboard_voice_assistant_text(lcd_dashboard_voice_state_t state)
{
    switch (state) {
    case LCD_DASHBOARD_VOICE_REC:
        return "REC";
    case LCD_DASHBOARD_VOICE_PLAY:
        return "PLAY";
    case LCD_DASHBOARD_VOICE_ERR:
        return "ERROR";
    case LCD_DASHBOARD_VOICE_WAKE:
    case LCD_DASHBOARD_VOICE_WAIT:
        return "LISTEN";
    case LCD_DASHBOARD_VOICE_LISTEN:
    default:
        return "IDLE";
    }
}

static void lcd_lvgl_set_static_text(lv_obj_t *label, const char *text)
{
    if (label != NULL && text != NULL) {
        lv_label_set_text_static(label, text);
    }
}

/* I4 source maps: index 0 is transparent, 1 white, 2 blue-gray, 3 mint, 4 pink.
 * Each logical grid cell is four physical pixels wide; rows are emitted twice. */
#define CAT_NIBBLE_PAIR_0 0x00
#define CAT_NIBBLE_PAIR_1 0x11
#define CAT_NIBBLE_PAIR_2 0x22
#define CAT_NIBBLE_PAIR_3 0x33
#define CAT_NIBBLE_PAIR_4 0x44
#define CAT_BLOCK(c) CAT_NIBBLE_PAIR_##c, CAT_NIBBLE_PAIR_##c,
#define CAT_ROW16(a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p) \
    CAT_BLOCK(a) CAT_BLOCK(b) CAT_BLOCK(c) CAT_BLOCK(d) \
    CAT_BLOCK(e) CAT_BLOCK(f) CAT_BLOCK(g) CAT_BLOCK(h) \
    CAT_BLOCK(i) CAT_BLOCK(j) CAT_BLOCK(k) CAT_BLOCK(l) \
    CAT_BLOCK(m) CAT_BLOCK(n) CAT_BLOCK(o) CAT_BLOCK(p)
#define CAT_REPEAT2(row) row row
#define CAT_BLOCK_X2(c) CAT_NIBBLE_PAIR_##c, CAT_NIBBLE_PAIR_##c, \
                        CAT_NIBBLE_PAIR_##c, CAT_NIBBLE_PAIR_##c,
#define CAT_ROW16_X2(a, b, c, d, e, f, g, h, i, j, k, l, m, n, o, p) \
    CAT_BLOCK_X2(a) CAT_BLOCK_X2(b) CAT_BLOCK_X2(c) CAT_BLOCK_X2(d) \
    CAT_BLOCK_X2(e) CAT_BLOCK_X2(f) CAT_BLOCK_X2(g) CAT_BLOCK_X2(h) \
    CAT_BLOCK_X2(i) CAT_BLOCK_X2(j) CAT_BLOCK_X2(k) CAT_BLOCK_X2(l) \
    CAT_BLOCK_X2(m) CAT_BLOCK_X2(n) CAT_BLOCK_X2(o) CAT_BLOCK_X2(p)
#define CAT_REPEAT4(row) row row row row

/* The fixed 16 x 24 pixel-art grid is expanded to the 64 x 48 I4 source. */
#define CAT_ROW_CLEAR CAT_ROW16(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)
#define CAT_ROW_EAR_TIP CAT_ROW16(0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0)
#define CAT_ROW_EAR_UPPER CAT_ROW16(0, 0, 1, 3, 1, 0, 0, 0, 0, 0, 0, 1, 3, 1, 0, 0)
#define CAT_ROW_EAR_BASE CAT_ROW16(0, 1, 3, 3, 1, 1, 0, 0, 0, 0, 1, 1, 3, 3, 1, 0)
#define CAT_ROW_FACE_TOP CAT_ROW16(0, 1, 3, 3, 1, 1, 1, 1, 1, 1, 1, 1, 3, 3, 1, 0)
#define CAT_ROW_FACE CAT_ROW16(0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0)
#define CAT_ROW_LISTEN_EYES CAT_ROW16(0, 1, 1, 1, 1, 2, 2, 1, 1, 2, 2, 1, 1, 1, 1, 0)
#define CAT_ROW_OPEN_EYES CAT_ROW16(0, 1, 1, 1, 1, 2, 1, 1, 1, 1, 2, 1, 1, 1, 1, 0)
#define CAT_ROW_ERROR_EYES_A CAT_ROW16(0, 1, 1, 1, 1, 2, 1, 1, 1, 1, 2, 1, 1, 1, 1, 0)
#define CAT_ROW_ERROR_EYES_B CAT_ROW16(0, 1, 1, 1, 1, 1, 2, 1, 1, 2, 1, 1, 1, 1, 1, 0)
#define CAT_ROW_CHEEKS CAT_ROW16(2, 2, 1, 4, 4, 1, 1, 1, 1, 1, 1, 4, 4, 1, 2, 2)
#define CAT_ROW_NOSE CAT_ROW16(0, 2, 1, 1, 1, 1, 1, 4, 1, 1, 1, 1, 1, 1, 2, 0)
#define CAT_ROW_MOUTH_CLOSED CAT_ROW16(0, 0, 1, 1, 1, 1, 1, 2, 1, 1, 1, 1, 1, 1, 0, 0)
#define CAT_ROW_MOUTH_OPEN CAT_ROW16(0, 0, 1, 1, 1, 1, 1, 2, 2, 1, 1, 1, 1, 1, 0, 0)
#define CAT_ROW_FACE_LOWER CAT_ROW16(0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0)
#define CAT_ROW_PAW CAT_ROW16(0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0)
#define CAT_ROW_PAW_PAD CAT_ROW16(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 3, 0)
#define CAT_ROW_CLEAR_X2 CAT_ROW16_X2(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)
#define CAT_ROW_EAR_TIP_X2 CAT_ROW16_X2(0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0)
#define CAT_ROW_EAR_UPPER_X2 CAT_ROW16_X2(0, 0, 1, 3, 1, 0, 0, 0, 0, 0, 0, 1, 3, 1, 0, 0)
#define CAT_ROW_EAR_BASE_X2 CAT_ROW16_X2(0, 1, 3, 3, 1, 1, 0, 0, 0, 0, 1, 1, 3, 3, 1, 0)
#define CAT_ROW_FACE_TOP_X2 CAT_ROW16_X2(0, 1, 3, 3, 1, 1, 1, 1, 1, 1, 1, 1, 3, 3, 1, 0)
#define CAT_ROW_FACE_X2 CAT_ROW16_X2(0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0)
#define CAT_ROW_LISTEN_EYES_X2 CAT_ROW16_X2(0, 1, 1, 1, 1, 2, 2, 1, 1, 2, 2, 1, 1, 1, 1, 0)
#define CAT_ROW_OPEN_EYES_X2 CAT_ROW16_X2(0, 1, 1, 1, 1, 2, 1, 1, 1, 1, 2, 1, 1, 1, 1, 0)
#define CAT_ROW_ERROR_EYES_A_X2 CAT_ROW16_X2(0, 1, 1, 1, 1, 2, 1, 1, 1, 1, 2, 1, 1, 1, 1, 0)
#define CAT_ROW_ERROR_EYES_B_X2 CAT_ROW16_X2(0, 1, 1, 1, 1, 1, 2, 1, 1, 2, 1, 1, 1, 1, 1, 0)
#define CAT_ROW_CHEEKS_X2 CAT_ROW16_X2(2, 2, 1, 4, 4, 1, 1, 1, 1, 1, 1, 4, 4, 1, 2, 2)
#define CAT_ROW_NOSE_X2 CAT_ROW16_X2(0, 2, 1, 1, 1, 1, 1, 4, 1, 1, 1, 1, 1, 1, 2, 0)
#define CAT_ROW_MOUTH_CLOSED_X2 CAT_ROW16_X2(0, 0, 1, 1, 1, 1, 1, 2, 1, 1, 1, 1, 1, 1, 0, 0)
#define CAT_ROW_MOUTH_OPEN_X2 CAT_ROW16_X2(0, 0, 1, 1, 1, 1, 1, 2, 2, 1, 1, 1, 1, 1, 0, 0)
#define CAT_ROW_FACE_LOWER_X2 CAT_ROW16_X2(0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0)
#define CAT_ROW_PAW_X2 CAT_ROW16_X2(0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0)
#define CAT_ROW_PAW_PAD_X2 CAT_ROW16_X2(0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 3, 0)
#define CAT_I4_PALETTE \
    0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, \
    0x54, 0x3d, 0x2a, 0xff, 0xa6, 0xd2, 0x79, 0xff, \
    0xb5, 0x9b, 0xf4, 0xff, 0x00, 0x00, 0x00, 0x00, \
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, \
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, \
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, \
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, \
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,

static const uint8_t s_cat_listen_map[] __attribute__((aligned(4))) = {
    CAT_I4_PALETTE
    CAT_REPEAT2(CAT_ROW_CLEAR) CAT_REPEAT2(CAT_ROW_EAR_TIP)
    CAT_REPEAT2(CAT_ROW_EAR_UPPER) CAT_REPEAT2(CAT_ROW_EAR_BASE)
    CAT_REPEAT2(CAT_ROW_FACE_TOP) CAT_REPEAT2(CAT_ROW_FACE)
    CAT_REPEAT2(CAT_ROW_FACE) CAT_REPEAT2(CAT_ROW_LISTEN_EYES)
    CAT_REPEAT2(CAT_ROW_FACE) CAT_REPEAT2(CAT_ROW_CHEEKS)
    CAT_REPEAT2(CAT_ROW_NOSE) CAT_REPEAT2(CAT_ROW_MOUTH_CLOSED)
    CAT_REPEAT2(CAT_ROW_FACE_LOWER) CAT_REPEAT2(CAT_ROW_FACE_LOWER)
    CAT_REPEAT2(CAT_ROW_FACE_LOWER) CAT_REPEAT2(CAT_ROW_FACE_LOWER)
    CAT_REPEAT2(CAT_ROW_CLEAR) CAT_REPEAT2(CAT_ROW_CLEAR)
    CAT_REPEAT2(CAT_ROW_CLEAR) CAT_REPEAT2(CAT_ROW_CLEAR)
    CAT_REPEAT2(CAT_ROW_CLEAR) CAT_REPEAT2(CAT_ROW_CLEAR)
    CAT_REPEAT2(CAT_ROW_CLEAR) CAT_REPEAT2(CAT_ROW_CLEAR)
};

static const uint8_t s_cat_open_map[] __attribute__((aligned(4))) = {
    CAT_I4_PALETTE
    CAT_REPEAT2(CAT_ROW_CLEAR) CAT_REPEAT2(CAT_ROW_EAR_TIP)
    CAT_REPEAT2(CAT_ROW_EAR_UPPER) CAT_REPEAT2(CAT_ROW_EAR_BASE)
    CAT_REPEAT2(CAT_ROW_FACE_TOP) CAT_REPEAT2(CAT_ROW_FACE)
    CAT_REPEAT2(CAT_ROW_FACE) CAT_REPEAT2(CAT_ROW_OPEN_EYES)
    CAT_REPEAT2(CAT_ROW_OPEN_EYES) CAT_REPEAT2(CAT_ROW_CHEEKS)
    CAT_REPEAT2(CAT_ROW_NOSE) CAT_REPEAT2(CAT_ROW_MOUTH_CLOSED)
    CAT_REPEAT2(CAT_ROW_FACE_LOWER) CAT_REPEAT2(CAT_ROW_FACE_LOWER)
    CAT_REPEAT2(CAT_ROW_FACE_LOWER) CAT_REPEAT2(CAT_ROW_FACE_LOWER)
    CAT_REPEAT2(CAT_ROW_CLEAR) CAT_REPEAT2(CAT_ROW_CLEAR)
    CAT_REPEAT2(CAT_ROW_CLEAR) CAT_REPEAT2(CAT_ROW_CLEAR)
    CAT_REPEAT2(CAT_ROW_CLEAR) CAT_REPEAT2(CAT_ROW_CLEAR)
    CAT_REPEAT2(CAT_ROW_CLEAR) CAT_REPEAT2(CAT_ROW_CLEAR)
};

static const uint8_t s_cat_rec_map[] __attribute__((aligned(4))) = {
    CAT_I4_PALETTE
    CAT_REPEAT2(CAT_ROW_CLEAR) CAT_REPEAT2(CAT_ROW_EAR_TIP)
    CAT_REPEAT2(CAT_ROW_EAR_UPPER) CAT_REPEAT2(CAT_ROW_EAR_BASE)
    CAT_REPEAT2(CAT_ROW_FACE_TOP) CAT_REPEAT2(CAT_ROW_FACE)
    CAT_REPEAT2(CAT_ROW_FACE) CAT_REPEAT2(CAT_ROW_OPEN_EYES)
    CAT_REPEAT2(CAT_ROW_OPEN_EYES) CAT_REPEAT2(CAT_ROW_CHEEKS)
    CAT_REPEAT2(CAT_ROW_NOSE) CAT_REPEAT2(CAT_ROW_MOUTH_CLOSED)
    CAT_REPEAT2(CAT_ROW_FACE_LOWER) CAT_REPEAT2(CAT_ROW_FACE_LOWER)
    CAT_REPEAT2(CAT_ROW_PAW) CAT_REPEAT2(CAT_ROW_PAW_PAD)
    CAT_REPEAT2(CAT_ROW_CLEAR) CAT_REPEAT2(CAT_ROW_CLEAR)
    CAT_REPEAT2(CAT_ROW_CLEAR) CAT_REPEAT2(CAT_ROW_CLEAR)
    CAT_REPEAT2(CAT_ROW_CLEAR) CAT_REPEAT2(CAT_ROW_CLEAR)
    CAT_REPEAT2(CAT_ROW_CLEAR) CAT_REPEAT2(CAT_ROW_CLEAR)
};

static const uint8_t s_cat_play_map[] __attribute__((aligned(4))) = {
    CAT_I4_PALETTE
    CAT_REPEAT2(CAT_ROW_CLEAR) CAT_REPEAT2(CAT_ROW_EAR_TIP)
    CAT_REPEAT2(CAT_ROW_EAR_UPPER) CAT_REPEAT2(CAT_ROW_EAR_BASE)
    CAT_REPEAT2(CAT_ROW_FACE_TOP) CAT_REPEAT2(CAT_ROW_FACE)
    CAT_REPEAT2(CAT_ROW_FACE) CAT_REPEAT2(CAT_ROW_OPEN_EYES)
    CAT_REPEAT2(CAT_ROW_OPEN_EYES) CAT_REPEAT2(CAT_ROW_CHEEKS)
    CAT_REPEAT2(CAT_ROW_NOSE) CAT_REPEAT2(CAT_ROW_MOUTH_OPEN)
    CAT_REPEAT2(CAT_ROW_MOUTH_OPEN) CAT_REPEAT2(CAT_ROW_FACE_LOWER)
    CAT_REPEAT2(CAT_ROW_FACE_LOWER) CAT_REPEAT2(CAT_ROW_FACE_LOWER)
    CAT_REPEAT2(CAT_ROW_CLEAR) CAT_REPEAT2(CAT_ROW_CLEAR)
    CAT_REPEAT2(CAT_ROW_CLEAR) CAT_REPEAT2(CAT_ROW_CLEAR)
    CAT_REPEAT2(CAT_ROW_CLEAR) CAT_REPEAT2(CAT_ROW_CLEAR)
    CAT_REPEAT2(CAT_ROW_CLEAR) CAT_REPEAT2(CAT_ROW_CLEAR)
};

static const uint8_t s_cat_error_map[] __attribute__((aligned(4))) = {
    CAT_I4_PALETTE
    CAT_REPEAT2(CAT_ROW_CLEAR) CAT_REPEAT2(CAT_ROW_EAR_TIP)
    CAT_REPEAT2(CAT_ROW_EAR_UPPER) CAT_REPEAT2(CAT_ROW_EAR_BASE)
    CAT_REPEAT2(CAT_ROW_FACE_TOP) CAT_REPEAT2(CAT_ROW_FACE)
    CAT_REPEAT2(CAT_ROW_FACE) CAT_REPEAT2(CAT_ROW_ERROR_EYES_A)
    CAT_REPEAT2(CAT_ROW_ERROR_EYES_B) CAT_REPEAT2(CAT_ROW_CHEEKS)
    CAT_REPEAT2(CAT_ROW_NOSE) CAT_REPEAT2(CAT_ROW_MOUTH_CLOSED)
    CAT_REPEAT2(CAT_ROW_FACE_LOWER) CAT_REPEAT2(CAT_ROW_FACE_LOWER)
    CAT_REPEAT2(CAT_ROW_FACE_LOWER) CAT_REPEAT2(CAT_ROW_FACE_LOWER)
    CAT_REPEAT2(CAT_ROW_CLEAR) CAT_REPEAT2(CAT_ROW_CLEAR)
    CAT_REPEAT2(CAT_ROW_CLEAR) CAT_REPEAT2(CAT_ROW_CLEAR)
    CAT_REPEAT2(CAT_ROW_CLEAR) CAT_REPEAT2(CAT_ROW_CLEAR)
    CAT_REPEAT2(CAT_ROW_CLEAR) CAT_REPEAT2(CAT_ROW_CLEAR)
};

/* Voice-screen frames are exact 2x nearest-neighbor expansions of the source I4 cats. */
static const uint8_t s_cat_listen_voice_map[] __attribute__((aligned(4))) = {
    CAT_I4_PALETTE
    CAT_REPEAT4(CAT_ROW_CLEAR_X2) CAT_REPEAT4(CAT_ROW_EAR_TIP_X2)
    CAT_REPEAT4(CAT_ROW_EAR_UPPER_X2) CAT_REPEAT4(CAT_ROW_EAR_BASE_X2)
    CAT_REPEAT4(CAT_ROW_FACE_TOP_X2) CAT_REPEAT4(CAT_ROW_FACE_X2)
    CAT_REPEAT4(CAT_ROW_FACE_X2) CAT_REPEAT4(CAT_ROW_LISTEN_EYES_X2)
    CAT_REPEAT4(CAT_ROW_FACE_X2) CAT_REPEAT4(CAT_ROW_CHEEKS_X2)
    CAT_REPEAT4(CAT_ROW_NOSE_X2) CAT_REPEAT4(CAT_ROW_MOUTH_CLOSED_X2)
    CAT_REPEAT4(CAT_ROW_FACE_LOWER_X2) CAT_REPEAT4(CAT_ROW_FACE_LOWER_X2)
    CAT_REPEAT4(CAT_ROW_FACE_LOWER_X2) CAT_REPEAT4(CAT_ROW_FACE_LOWER_X2)
    CAT_REPEAT4(CAT_ROW_CLEAR_X2) CAT_REPEAT4(CAT_ROW_CLEAR_X2)
    CAT_REPEAT4(CAT_ROW_CLEAR_X2) CAT_REPEAT4(CAT_ROW_CLEAR_X2)
    CAT_REPEAT4(CAT_ROW_CLEAR_X2) CAT_REPEAT4(CAT_ROW_CLEAR_X2)
    CAT_REPEAT4(CAT_ROW_CLEAR_X2) CAT_REPEAT4(CAT_ROW_CLEAR_X2)
};

static const uint8_t s_cat_open_voice_map[] __attribute__((aligned(4))) = {
    CAT_I4_PALETTE
    CAT_REPEAT4(CAT_ROW_CLEAR_X2) CAT_REPEAT4(CAT_ROW_EAR_TIP_X2)
    CAT_REPEAT4(CAT_ROW_EAR_UPPER_X2) CAT_REPEAT4(CAT_ROW_EAR_BASE_X2)
    CAT_REPEAT4(CAT_ROW_FACE_TOP_X2) CAT_REPEAT4(CAT_ROW_FACE_X2)
    CAT_REPEAT4(CAT_ROW_FACE_X2) CAT_REPEAT4(CAT_ROW_OPEN_EYES_X2)
    CAT_REPEAT4(CAT_ROW_OPEN_EYES_X2) CAT_REPEAT4(CAT_ROW_CHEEKS_X2)
    CAT_REPEAT4(CAT_ROW_NOSE_X2) CAT_REPEAT4(CAT_ROW_MOUTH_CLOSED_X2)
    CAT_REPEAT4(CAT_ROW_FACE_LOWER_X2) CAT_REPEAT4(CAT_ROW_FACE_LOWER_X2)
    CAT_REPEAT4(CAT_ROW_FACE_LOWER_X2) CAT_REPEAT4(CAT_ROW_FACE_LOWER_X2)
    CAT_REPEAT4(CAT_ROW_CLEAR_X2) CAT_REPEAT4(CAT_ROW_CLEAR_X2)
    CAT_REPEAT4(CAT_ROW_CLEAR_X2) CAT_REPEAT4(CAT_ROW_CLEAR_X2)
    CAT_REPEAT4(CAT_ROW_CLEAR_X2) CAT_REPEAT4(CAT_ROW_CLEAR_X2)
    CAT_REPEAT4(CAT_ROW_CLEAR_X2) CAT_REPEAT4(CAT_ROW_CLEAR_X2)
};

static const uint8_t s_cat_rec_voice_map[] __attribute__((aligned(4))) = {
    CAT_I4_PALETTE
    CAT_REPEAT4(CAT_ROW_CLEAR_X2) CAT_REPEAT4(CAT_ROW_EAR_TIP_X2)
    CAT_REPEAT4(CAT_ROW_EAR_UPPER_X2) CAT_REPEAT4(CAT_ROW_EAR_BASE_X2)
    CAT_REPEAT4(CAT_ROW_FACE_TOP_X2) CAT_REPEAT4(CAT_ROW_FACE_X2)
    CAT_REPEAT4(CAT_ROW_FACE_X2) CAT_REPEAT4(CAT_ROW_OPEN_EYES_X2)
    CAT_REPEAT4(CAT_ROW_OPEN_EYES_X2) CAT_REPEAT4(CAT_ROW_CHEEKS_X2)
    CAT_REPEAT4(CAT_ROW_NOSE_X2) CAT_REPEAT4(CAT_ROW_MOUTH_CLOSED_X2)
    CAT_REPEAT4(CAT_ROW_FACE_LOWER_X2) CAT_REPEAT4(CAT_ROW_FACE_LOWER_X2)
    CAT_REPEAT4(CAT_ROW_PAW_X2) CAT_REPEAT4(CAT_ROW_PAW_PAD_X2)
    CAT_REPEAT4(CAT_ROW_CLEAR_X2) CAT_REPEAT4(CAT_ROW_CLEAR_X2)
    CAT_REPEAT4(CAT_ROW_CLEAR_X2) CAT_REPEAT4(CAT_ROW_CLEAR_X2)
    CAT_REPEAT4(CAT_ROW_CLEAR_X2) CAT_REPEAT4(CAT_ROW_CLEAR_X2)
    CAT_REPEAT4(CAT_ROW_CLEAR_X2) CAT_REPEAT4(CAT_ROW_CLEAR_X2)
};

static const uint8_t s_cat_play_voice_map[] __attribute__((aligned(4))) = {
    CAT_I4_PALETTE
    CAT_REPEAT4(CAT_ROW_CLEAR_X2) CAT_REPEAT4(CAT_ROW_EAR_TIP_X2)
    CAT_REPEAT4(CAT_ROW_EAR_UPPER_X2) CAT_REPEAT4(CAT_ROW_EAR_BASE_X2)
    CAT_REPEAT4(CAT_ROW_FACE_TOP_X2) CAT_REPEAT4(CAT_ROW_FACE_X2)
    CAT_REPEAT4(CAT_ROW_FACE_X2) CAT_REPEAT4(CAT_ROW_OPEN_EYES_X2)
    CAT_REPEAT4(CAT_ROW_OPEN_EYES_X2) CAT_REPEAT4(CAT_ROW_CHEEKS_X2)
    CAT_REPEAT4(CAT_ROW_NOSE_X2) CAT_REPEAT4(CAT_ROW_MOUTH_OPEN_X2)
    CAT_REPEAT4(CAT_ROW_MOUTH_OPEN_X2) CAT_REPEAT4(CAT_ROW_FACE_LOWER_X2)
    CAT_REPEAT4(CAT_ROW_FACE_LOWER_X2) CAT_REPEAT4(CAT_ROW_FACE_LOWER_X2)
    CAT_REPEAT4(CAT_ROW_CLEAR_X2) CAT_REPEAT4(CAT_ROW_CLEAR_X2)
    CAT_REPEAT4(CAT_ROW_CLEAR_X2) CAT_REPEAT4(CAT_ROW_CLEAR_X2)
    CAT_REPEAT4(CAT_ROW_CLEAR_X2) CAT_REPEAT4(CAT_ROW_CLEAR_X2)
    CAT_REPEAT4(CAT_ROW_CLEAR_X2) CAT_REPEAT4(CAT_ROW_CLEAR_X2)
};

static const uint8_t s_cat_error_voice_map[] __attribute__((aligned(4))) = {
    CAT_I4_PALETTE
    CAT_REPEAT4(CAT_ROW_CLEAR_X2) CAT_REPEAT4(CAT_ROW_EAR_TIP_X2)
    CAT_REPEAT4(CAT_ROW_EAR_UPPER_X2) CAT_REPEAT4(CAT_ROW_EAR_BASE_X2)
    CAT_REPEAT4(CAT_ROW_FACE_TOP_X2) CAT_REPEAT4(CAT_ROW_FACE_X2)
    CAT_REPEAT4(CAT_ROW_FACE_X2) CAT_REPEAT4(CAT_ROW_ERROR_EYES_A_X2)
    CAT_REPEAT4(CAT_ROW_ERROR_EYES_B_X2) CAT_REPEAT4(CAT_ROW_CHEEKS_X2)
    CAT_REPEAT4(CAT_ROW_NOSE_X2) CAT_REPEAT4(CAT_ROW_MOUTH_CLOSED_X2)
    CAT_REPEAT4(CAT_ROW_FACE_LOWER_X2) CAT_REPEAT4(CAT_ROW_FACE_LOWER_X2)
    CAT_REPEAT4(CAT_ROW_FACE_LOWER_X2) CAT_REPEAT4(CAT_ROW_FACE_LOWER_X2)
    CAT_REPEAT4(CAT_ROW_CLEAR_X2) CAT_REPEAT4(CAT_ROW_CLEAR_X2)
    CAT_REPEAT4(CAT_ROW_CLEAR_X2) CAT_REPEAT4(CAT_ROW_CLEAR_X2)
    CAT_REPEAT4(CAT_ROW_CLEAR_X2) CAT_REPEAT4(CAT_ROW_CLEAR_X2)
    CAT_REPEAT4(CAT_ROW_CLEAR_X2) CAT_REPEAT4(CAT_ROW_CLEAR_X2)
};

_Static_assert(sizeof(s_cat_listen_map) == LCD_CAT_IMAGE_DATA_BYTES, "listen frame size");
_Static_assert(sizeof(s_cat_open_map) == LCD_CAT_IMAGE_DATA_BYTES, "open frame size");
_Static_assert(sizeof(s_cat_rec_map) == LCD_CAT_IMAGE_DATA_BYTES, "recording frame size");
_Static_assert(sizeof(s_cat_play_map) == LCD_CAT_IMAGE_DATA_BYTES, "play frame size");
_Static_assert(sizeof(s_cat_error_map) == LCD_CAT_IMAGE_DATA_BYTES, "error frame size");
_Static_assert(sizeof(s_cat_listen_voice_map) == LCD_CAT_VOICE_IMAGE_DATA_BYTES, "voice listen frame size");
_Static_assert(sizeof(s_cat_open_voice_map) == LCD_CAT_VOICE_IMAGE_DATA_BYTES, "voice open frame size");
_Static_assert(sizeof(s_cat_rec_voice_map) == LCD_CAT_VOICE_IMAGE_DATA_BYTES, "voice recording frame size");
_Static_assert(sizeof(s_cat_play_voice_map) == LCD_CAT_VOICE_IMAGE_DATA_BYTES, "voice play frame size");
_Static_assert(sizeof(s_cat_error_voice_map) == LCD_CAT_VOICE_IMAGE_DATA_BYTES, "voice error frame size");

#define CAT_IMAGE_DSC(map) \
    { \
        .header = { \
            .magic = LV_IMAGE_HEADER_MAGIC, \
            .cf = LV_COLOR_FORMAT_I4, \
            .flags = 0, \
            .w = LCD_CAT_IMAGE_WIDTH, \
            .h = LCD_CAT_IMAGE_HEIGHT, \
            .stride = LCD_CAT_IMAGE_STRIDE, \
        }, \
        .data_size = sizeof(map), \
        .data = map, \
        .reserved = NULL, \
    }

static const lv_image_dsc_t s_cat_listen = CAT_IMAGE_DSC(s_cat_listen_map);
static const lv_image_dsc_t s_cat_open = CAT_IMAGE_DSC(s_cat_open_map);
static const lv_image_dsc_t s_cat_rec = CAT_IMAGE_DSC(s_cat_rec_map);
static const lv_image_dsc_t s_cat_play = CAT_IMAGE_DSC(s_cat_play_map);
static const lv_image_dsc_t s_cat_error = CAT_IMAGE_DSC(s_cat_error_map);

#define CAT_VOICE_IMAGE_DSC(map) \
    { \
        .header = { \
            .magic = LV_IMAGE_HEADER_MAGIC, \
            .cf = LV_COLOR_FORMAT_I4, \
            .flags = 0, \
            .w = LCD_CAT_VOICE_IMAGE_WIDTH, \
            .h = LCD_CAT_VOICE_IMAGE_HEIGHT, \
            .stride = LCD_CAT_VOICE_IMAGE_STRIDE, \
        }, \
        .data_size = sizeof(map), \
        .data = map, \
        .reserved = NULL, \
    }

static const lv_image_dsc_t s_cat_listen_voice = CAT_VOICE_IMAGE_DSC(s_cat_listen_voice_map);
static const lv_image_dsc_t s_cat_open_voice = CAT_VOICE_IMAGE_DSC(s_cat_open_voice_map);
static const lv_image_dsc_t s_cat_rec_voice = CAT_VOICE_IMAGE_DSC(s_cat_rec_voice_map);
static const lv_image_dsc_t s_cat_play_voice = CAT_VOICE_IMAGE_DSC(s_cat_play_voice_map);
static const lv_image_dsc_t s_cat_error_voice = CAT_VOICE_IMAGE_DSC(s_cat_error_voice_map);

static void lcd_cat_disable_after_allocation_failure(void)
{
    if (s_cat_animation_timer != NULL) {
        lv_timer_delete(s_cat_animation_timer);
        s_cat_animation_timer = NULL;
    }
    if (s_cat_image != NULL) {
        lv_obj_delete(s_cat_image);
        s_cat_image = NULL;
    }
    if (s_cat_hit_area != NULL) {
        lv_obj_delete(s_cat_hit_area);
        s_cat_hit_area = NULL;
    }
    s_cat_frame = NULL;
    s_cat_voice_state_valid = false;
    s_cat_disabled = true;
    ESP_LOGE(TAG, "LCD_CAT_DISABLED reason=allocation_failed");
}

static void lcd_cat_set_frame(const lv_image_dsc_t *frame, int16_t offset_x, int16_t offset_y)
{
    if (s_cat_image == NULL || frame == NULL) {
        return;
    }
    if (s_cat_frame != frame) {
        lv_image_set_src(s_cat_image, frame);
        s_cat_frame = frame;
    }
    lv_obj_set_pos(s_cat_image, LCD_CAT_IMAGE_X + offset_x, LCD_CAT_IMAGE_Y + offset_y);
}

static void lcd_cat_apply_static_state(lcd_dashboard_voice_state_t state)
{
    switch (state) {
    case LCD_DASHBOARD_VOICE_WAKE:
    case LCD_DASHBOARD_VOICE_WAIT:
        lcd_cat_set_frame(&s_cat_open, 0, 0);
        break;
    case LCD_DASHBOARD_VOICE_REC:
        lcd_cat_set_frame(&s_cat_rec, 0, 0);
        break;
    case LCD_DASHBOARD_VOICE_PLAY:
        lcd_cat_set_frame(&s_cat_open, 0, 0);
        break;
    case LCD_DASHBOARD_VOICE_ERR:
        lcd_cat_set_frame(&s_cat_error, 0, 0);
        break;
    case LCD_DASHBOARD_VOICE_LISTEN:
    default:
        lcd_cat_set_frame(&s_cat_listen, 0, 0);
        break;
    }
}

static bool lcd_cat_create(lv_obj_t *screen)
{
    s_cat_disabled = false;
    s_cat_image = lv_image_create(screen);
    if (s_cat_image == NULL) {
        lcd_cat_disable_after_allocation_failure();
        return false;
    }
    s_cat_frame = NULL;
    lcd_cat_set_frame(&s_cat_listen, 0, 0);
    lv_obj_add_flag(s_cat_image, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_cat_image, lcd_dashboard_cat_click_cb, LV_EVENT_CLICKED, NULL);

    s_cat_hit_area = lv_obj_create(screen);
    if (s_cat_hit_area == NULL) {
        lcd_cat_disable_after_allocation_failure();
        return false;
    }
    lv_obj_remove_style_all(s_cat_hit_area);
    lv_obj_set_size(s_cat_hit_area,
                    LCD_DASHBOARD_CAT_HIT_WIDTH,
                    LCD_DASHBOARD_CAT_HIT_HEIGHT);
    lv_obj_set_pos(s_cat_hit_area, LCD_DASHBOARD_CAT_HIT_X, LCD_DASHBOARD_CAT_HIT_Y);
    lv_obj_set_style_bg_opa(s_cat_hit_area, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_clear_flag(s_cat_hit_area, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_cat_hit_area, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_cat_hit_area, lcd_dashboard_cat_click_cb, LV_EVENT_CLICKED, NULL);

    s_cat_animation_timer = lv_timer_create(lcd_cat_animation_timer_cb,
                                             LCD_CAT_ANIMATION_PERIOD_MS,
                                             NULL);
    if (s_cat_animation_timer == NULL) {
        lcd_cat_disable_after_allocation_failure();
        return false;
    }
    lv_timer_pause(s_cat_animation_timer);
    return true;
}

static void lcd_cat_animation_timer_cb(lv_timer_t *timer)
{
    if (s_cat_disabled || timer == NULL) {
        if (timer != NULL) {
            lv_timer_pause(timer);
        }
        return;
    }
    if (s_cat_voice_state == LCD_DASHBOARD_VOICE_WAKE) {
        s_cat_animation_frame++;
        if (s_cat_animation_frame >= LCD_CAT_WAKE_FRAMES) {
            lcd_cat_set_frame(&s_cat_open, 0, 0);
            lv_timer_pause(timer);
            return;
        }
        const int16_t head_offset = s_cat_animation_frame < 3U ? -2 :
                                    (s_cat_animation_frame < 5U ? -1 : 0);
        lcd_cat_set_frame(&s_cat_open, 0, head_offset);
        return;
    }

    if (s_cat_voice_state == LCD_DASHBOARD_VOICE_REC) {
        s_cat_animation_frame++;
        if (s_cat_animation_frame >= LCD_CAT_RECORDING_FRAMES) {
            lcd_cat_set_frame(&s_cat_rec, 0, 0);
            lv_timer_pause(timer);
            return;
        }
        const int16_t paw_shift = (s_cat_animation_frame & 1U) != 0U ? 1 : 0;
        lcd_cat_set_frame(&s_cat_rec, paw_shift, 0);
        return;
    }

    if (s_cat_speaker_active) {
        s_cat_play_frame++;
        if (s_cat_play_frame >= LCD_CAT_PLAY_MOUTH_PERIOD_FRAMES) {
            s_cat_play_frame = 0;
            s_cat_play_mouth_open = !s_cat_play_mouth_open;
            lcd_cat_set_frame(s_cat_play_mouth_open ? &s_cat_play : &s_cat_open, 0, 0);
        }
        return;
    }

    lv_timer_pause(timer);
}

static void lcd_cat_update_for_voice_state(lcd_dashboard_voice_state_t state,
                                           bool speaker_active)
{
    if (s_cat_disabled || s_cat_image == NULL) {
        return;
    }

    if (!s_cat_voice_state_valid || s_cat_voice_state != state) {
        s_cat_voice_state = state;
        s_cat_voice_state_valid = true;
        s_cat_animation_frame = 0;
        s_cat_play_frame = 0;
        s_cat_play_mouth_open = false;
        lcd_cat_apply_static_state(state);
        if (s_cat_animation_timer == NULL) {
            return;
        }

        if (state == LCD_DASHBOARD_VOICE_WAKE) {
            lcd_cat_set_frame(&s_cat_open, 0, -2);
            lv_timer_resume(s_cat_animation_timer);
        } else if (state == LCD_DASHBOARD_VOICE_REC && !speaker_active) {
            lv_timer_resume(s_cat_animation_timer);
        } else {
            lv_timer_pause(s_cat_animation_timer);
        }
    }

    if (s_cat_speaker_active == speaker_active || s_cat_animation_timer == NULL) {
        return;
    }
    s_cat_speaker_active = speaker_active;
    s_cat_play_frame = 0;
    if (speaker_active) {
        s_cat_play_mouth_open = true;
        lcd_cat_set_frame(&s_cat_play, 0, 0);
        lv_timer_resume(s_cat_animation_timer);
        ESP_LOGI(TAG, "LCD_CAT_AUDIO_SYNC start");
    } else {
        ESP_LOGI(TAG, "LCD_CAT_AUDIO_STOP timestamp=%lld", esp_timer_get_time());
        lv_timer_pause(s_cat_animation_timer);
        s_cat_play_mouth_open = false;
        lcd_cat_set_frame(&s_cat_open, 0, 0);
        ESP_LOGI(TAG, "LCD_CAT_CLOSED timestamp=%lld", esp_timer_get_time());
        ESP_LOGI(TAG, "LCD_CAT_AUDIO_SYNC stop");
    }
}

static lv_obj_t *lcd_lvgl_create_root(lv_obj_t *screen, bool opaque)
{
    lv_obj_t *root = lv_obj_create(screen);
    if (root == NULL) {
        return NULL;
    }
    lv_obj_set_pos(root, 0, 0);
    lv_obj_set_size(root, LCD_H_RES, LCD_V_RES);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_border_width(root, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(root, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(root, lv_color_hex(0x101820), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(root, opaque ? LV_OPA_COVER : LV_OPA_TRANSP, LV_PART_MAIN);
    return root;
}

static void lcd_voice_ui_disable(void)
{
    if (s_voice_animation_timer != NULL) {
        lv_timer_delete(s_voice_animation_timer);
        s_voice_animation_timer = NULL;
    }
    if (s_voice_root != NULL) {
        lv_obj_delete(s_voice_root);
        s_voice_root = NULL;
    }
    s_voice_cat_image = NULL;
    s_voice_cat_frame = NULL;
    s_voice_ui_visible = false;
    s_voice_ui_disabled = true;
    ESP_LOGE(TAG, "LCD_VOICE_UI_DISABLED reason=allocation_failed");
}

static void lcd_voice_ui_set_frame(const lv_image_dsc_t *frame, int16_t offset_x)
{
    if (s_voice_cat_image == NULL || frame == NULL) {
        return;
    }
    if (s_voice_cat_frame != frame) {
        lv_image_set_src(s_voice_cat_image, frame);
        s_voice_cat_frame = frame;
    }
    lv_obj_set_pos(s_voice_cat_image,
                   LCD_VOICE_CAT_IMAGE_X + (offset_x * LCD_CAT_VOICE_SCALE),
                   LCD_VOICE_CAT_IMAGE_Y);
}

static void lcd_voice_ui_apply_state(lcd_dashboard_voice_state_t state)
{
    switch (state) {
    case LCD_DASHBOARD_VOICE_WAKE:
        lcd_voice_ui_set_frame(&s_cat_listen_voice, 0);
        break;
    case LCD_DASHBOARD_VOICE_REC:
        lcd_voice_ui_set_frame(&s_cat_rec_voice, 0);
        break;
    case LCD_DASHBOARD_VOICE_WAIT:
        lcd_voice_ui_set_frame(&s_cat_open_voice, 0);
        break;
    case LCD_DASHBOARD_VOICE_PLAY:
        lcd_voice_ui_set_frame(&s_cat_open_voice, 0);
        break;
    case LCD_DASHBOARD_VOICE_ERR:
        lcd_voice_ui_set_frame(&s_cat_error_voice, 0);
        break;
    case LCD_DASHBOARD_VOICE_LISTEN:
    default:
        break;
    }
}

static bool lcd_voice_ui_create(lv_obj_t *screen)
{
    s_voice_ui_disabled = false;
    s_voice_root = lcd_lvgl_create_root(screen, true);
    if (s_voice_root == NULL) {
        lcd_voice_ui_disable();
        return false;
    }
    s_voice_cat_image = lv_image_create(s_voice_root);
    if (s_voice_cat_image == NULL) {
        lcd_voice_ui_disable();
        return false;
    }
    s_voice_cat_frame = NULL;
    lcd_voice_ui_set_frame(&s_cat_listen_voice, 0);
    s_voice_animation_timer = lv_timer_create(lcd_voice_animation_timer_cb,
                                               LCD_CAT_ANIMATION_PERIOD_MS,
                                               NULL);
    if (s_voice_animation_timer == NULL) {
        lcd_voice_ui_disable();
        return false;
    }
    lv_timer_pause(s_voice_animation_timer);
    lv_obj_add_flag(s_voice_root, LV_OBJ_FLAG_HIDDEN);
    return true;
}

static void lcd_voice_animation_timer_cb(lv_timer_t *timer)
{
    if (s_voice_ui_disabled || !s_voice_ui_visible || timer == NULL) {
        if (timer != NULL) {
            lv_timer_pause(timer);
        }
        return;
    }
    if (s_voice_ui_state == LCD_DASHBOARD_VOICE_REC) {
        s_voice_animation_frame++;
        if (s_voice_animation_frame >= LCD_CAT_RECORDING_FRAMES) {
            lcd_voice_ui_set_frame(&s_cat_rec_voice, 0);
            lv_timer_pause(timer);
            return;
        }
        lcd_voice_ui_set_frame(&s_cat_rec_voice,
                               (s_voice_animation_frame & 1U) != 0U ? 1 : 0);
        return;
    }
    if (s_voice_speaker_active) {
        s_voice_play_frame++;
        if (s_voice_play_frame >= LCD_CAT_PLAY_MOUTH_PERIOD_FRAMES) {
            s_voice_play_frame = 0;
            s_voice_play_mouth_open = !s_voice_play_mouth_open;
            lcd_voice_ui_set_frame(s_voice_play_mouth_open ? &s_cat_play_voice :
                                                               &s_cat_open_voice,
                                  0);
        }
        return;
    }
    lv_timer_pause(timer);
}

static void lcd_voice_ui_update(lcd_dashboard_voice_state_t state, bool speaker_active)
{
    const bool active = state != LCD_DASHBOARD_VOICE_LISTEN;
    if (s_voice_ui_disabled || s_voice_root == NULL || s_dashboard_root == NULL) {
        return;
    }
    if (!active) {
        if (s_voice_ui_visible) {
            lv_timer_pause(s_voice_animation_timer);
            lv_obj_add_flag(s_voice_root, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(s_dashboard_root, LV_OBJ_FLAG_HIDDEN);
            s_voice_ui_visible = false;
            ESP_LOGI(TAG, "LCD_UI_MODE voice->dashboard reason=voice_listen");
        }
        if (s_voice_speaker_active) {
            ESP_LOGI(TAG, "LCD_CAT_AUDIO_SYNC stop");
        }
        s_voice_ui_state = LCD_DASHBOARD_VOICE_LISTEN;
        s_voice_speaker_active = false;
        return;
    }

    if (!s_voice_ui_visible) {
        lv_timer_pause(s_cat_animation_timer);
        lv_obj_add_flag(s_dashboard_root, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_voice_root, LV_OBJ_FLAG_HIDDEN);
        s_voice_ui_visible = true;
        ESP_LOGI(TAG, "LCD_UI_MODE dashboard->voice generation=state_snapshot");
    }
    if (s_voice_ui_state != state) {
        s_voice_ui_state = state;
        s_voice_animation_frame = 0;
        s_voice_play_frame = 0;
        s_voice_play_mouth_open = false;
        lcd_voice_ui_apply_state(state);
        if (state == LCD_DASHBOARD_VOICE_REC && !speaker_active) {
            lv_timer_resume(s_voice_animation_timer);
        } else {
            lv_timer_pause(s_voice_animation_timer);
        }
    }

    if (s_voice_speaker_active == speaker_active) {
        return;
    }
    s_voice_speaker_active = speaker_active;
    s_voice_play_frame = 0;
    if (speaker_active) {
        s_voice_play_mouth_open = true;
        lcd_voice_ui_set_frame(&s_cat_play_voice, 0);
        lv_timer_resume(s_voice_animation_timer);
        ESP_LOGI(TAG, "LCD_CAT_AUDIO_SYNC start");
    } else {
        ESP_LOGI(TAG, "LCD_CAT_AUDIO_STOP timestamp=%lld", esp_timer_get_time());
        lv_timer_pause(s_voice_animation_timer);
        s_voice_play_mouth_open = false;
        lcd_voice_ui_set_frame(&s_cat_open_voice, 0);
        ESP_LOGI(TAG, "LCD_CAT_CLOSED timestamp=%lld", esp_timer_get_time());
        ESP_LOGI(TAG, "LCD_CAT_AUDIO_SYNC stop");
    }
}

static void lcd_lvgl_update_dashboard(const lcd_dashboard_snapshot_t *snapshot,
                                      bool update_dashboard_data)
{
    if (snapshot == NULL) {
        return;
    }

    lcd_voice_ui_update(snapshot->voice_state, snapshot->speaker_active);
    if (!s_voice_ui_visible) {
        lcd_cat_update_for_voice_state(snapshot->voice_state, snapshot->speaker_active);
    }
    if (!update_dashboard_data) {
        return;
    }

    lv_color_t air_color = lcd_dashboard_air_state_color(LCD_DASHBOARD_AIR_INIT);
    if (snapshot->bme_valid) {
        (void)snprintf(s_temp_text, sizeof(s_temp_text),
                       "TEMP   %.1f \xC2\xB0" "C", (double)snapshot->temperature_c);
        (void)snprintf(s_humidity_text, sizeof(s_humidity_text), "HUM    %.1f %%", (double)snapshot->humidity_percent);
        (void)snprintf(s_pressure_text, sizeof(s_pressure_text), "PRESS  %.1f hPa", (double)snapshot->pressure_hpa);
        if (snapshot->gas_valid) {
            (void)snprintf(s_gas_text, sizeof(s_gas_text), "GAS    %.1f kOhm",
                           (double)snapshot->gas_resistance_ohm / 1000.0);
        } else {
            (void)snprintf(s_gas_text, sizeof(s_gas_text), "GAS    --");
        }
        (void)snprintf(s_air_text, sizeof(s_air_text), "AIR    %s",
                       lcd_dashboard_air_state_name(snapshot->air_state));
        air_color = lcd_dashboard_air_state_color(snapshot->air_state);
    } else {
        (void)snprintf(s_temp_text, sizeof(s_temp_text), "TEMP   --.- \xC2\xB0" "C");
        (void)snprintf(s_humidity_text, sizeof(s_humidity_text), "HUM    --.- %%");
        (void)snprintf(s_pressure_text, sizeof(s_pressure_text), "PRESS  ----.- hPa");
        (void)snprintf(s_gas_text, sizeof(s_gas_text), "GAS    --.- kOhm");
        (void)snprintf(s_air_text, sizeof(s_air_text), "AIR    INIT");
    }

    const char *connection_text;
    lv_color_t connection_color;
    if (snapshot->network_ok && snapshot->gateway_ok) {
        connection_text = "Online";
        connection_color = lv_color_hex(0x79D2A6);
    } else if (snapshot->network_ok) {
        connection_text = "Connecting";
        connection_color = lv_color_hex(0xE5B75B);
    } else {
        connection_text = "Offline";
        connection_color = lv_color_hex(0xE56B6F);
    }
    (void)snprintf(s_network_text, sizeof(s_network_text), "%s", connection_text);
    lv_obj_set_style_text_color(s_air_label, air_color, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_network_label, connection_color, LV_PART_MAIN);
    lcd_lvgl_set_status_dot_color(s_air_status_dot, air_color);
    lcd_lvgl_set_status_dot_color(s_connection_status_dot, connection_color);
    (void)snprintf(s_voice_text, sizeof(s_voice_text), "%s",
                   lcd_dashboard_voice_assistant_text(snapshot->voice_state));

    lcd_lvgl_set_static_text(s_temp_label, s_temp_text);
    lcd_lvgl_set_static_text(s_humidity_label, s_humidity_text);
    lcd_lvgl_set_static_text(s_pressure_label, s_pressure_text);
    lcd_lvgl_set_static_text(s_gas_label, s_gas_text);
    lcd_lvgl_set_static_text(s_air_label, s_air_text);
    lcd_lvgl_set_static_text(s_network_label, s_network_text);
    lcd_lvgl_set_static_text(s_voice_label, s_voice_text);
}

/* This callback runs in the LVGL task. The provider copies only already-published snapshots. */
static void lcd_lvgl_dashboard_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    lcd_dashboard_snapshot_provider_t provider = NULL;
    void *provider_ctx = NULL;
    portENTER_CRITICAL(&s_snapshot_provider_lock);
    provider = s_snapshot_provider;
    provider_ctx = s_snapshot_provider_ctx;
    portEXIT_CRITICAL(&s_snapshot_provider_lock);

    lcd_dashboard_snapshot_t snapshot = {
        .voice_state = LCD_DASHBOARD_VOICE_LISTEN,
    };
    if (provider != NULL) {
        (void)provider(&snapshot, provider_ctx);
    }
    const int64_t now_ms = esp_timer_get_time() / 1000;
    const bool update_dashboard_data = s_last_dashboard_data_update_ms == 0 ||
                                       now_ms - s_last_dashboard_data_update_ms >=
                                           LCD_DASHBOARD_DATA_REFRESH_MS;
    if (update_dashboard_data) {
        s_last_dashboard_data_update_ms = now_ms;
    }
    lcd_lvgl_update_dashboard(&snapshot, update_dashboard_data);

    if (s_last_dashboard_log_ms == 0 ||
        now_ms - s_last_dashboard_log_ms >= LCD_DASHBOARD_LOG_PERIOD_MS) {
        s_last_dashboard_log_ms = now_ms;
        ESP_LOGI(TAG,
                 "LCD_DATA_UPDATE bme_valid=%d csi_valid=%d net=%d voice=%s",
                 snapshot.bme_valid ? 1 : 0,
                 snapshot.csi_valid ? 1 : 0,
                 snapshot.network_ok ? 1 : 0,
                 lcd_dashboard_voice_state_name(snapshot.voice_state));
    }
}

static void lcd_lvgl_log_heap(const char *stage)
{
    ESP_LOGI(MEM_TAG,
             "stage=%s internal_free=%u internal_largest=%u dma_free=%u dma_largest=%u",
             stage,
             (unsigned int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned int)heap_caps_get_free_size(MALLOC_CAP_DMA),
             (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
}

static void lcd_lvgl_log_memory(const char *stage)
{
    lv_mem_monitor_t monitor;
    lv_mem_monitor(&monitor);

    ESP_LOGI(MEM_TAG,
             "stage=%s total_size=%u free_size=%u free_biggest_size=%u max_used=%u used_pct=%u frag_pct=%u "
             "internal_free=%u internal_largest=%u dma_free=%u dma_largest=%u spiram_free=%u spiram_largest=%u",
             stage,
             (unsigned int)monitor.total_size,
             (unsigned int)monitor.free_size,
             (unsigned int)monitor.free_biggest_size,
             (unsigned int)monitor.max_used,
             (unsigned int)monitor.used_pct,
             (unsigned int)monitor.frag_pct,
             (unsigned int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned int)heap_caps_get_free_size(MALLOC_CAP_DMA),
             (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_DMA),
             (unsigned int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
}

/* LVGL invokes timer callbacks from lv_timer_handler() while the port lock is held. */
static void lcd_lvgl_memory_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    lcd_lvgl_log_memory("periodic");
}

static esp_err_t lcd_lvgl_create_status_ui(void)
{
    if (!lvgl_port_lock(1000)) {
        return ESP_ERR_TIMEOUT;
    }

    lv_obj_t *screen = lv_display_get_screen_active(s_display);
    /* A previous firmware/page may have left pixels in the panel framebuffer. Clean
     * the active LVGL screen before rebuilding the page so no retired objects remain. */
    lv_obj_clean(screen);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x101820), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);

    s_dashboard_root = lcd_lvgl_create_root(screen, false);
    if (s_dashboard_root == NULL) {
        lvgl_port_unlock();
        return ESP_ERR_NO_MEM;
    }

    const lv_color_t title_color = lv_color_hex(0xFFFFFF);
    const lv_color_t section_color = lv_color_hex(0x79D2A6);
    const lv_color_t value_color = lv_color_hex(0xDCE7EF);
    lv_obj_t *title = lcd_lvgl_create_label(s_dashboard_root, "SensAir C5", title_color, 12);
    lv_obj_t *environment = lcd_lvgl_create_label(s_dashboard_root, "ENVIRONMENT", section_color, 44);
    s_temp_label = lcd_lvgl_create_label(s_dashboard_root, s_temp_text, value_color, 72);
    s_humidity_label = lcd_lvgl_create_label(s_dashboard_root, s_humidity_text, value_color, 99);
    s_pressure_label = lcd_lvgl_create_label(s_dashboard_root, s_pressure_text, value_color, 126);
    s_gas_label = lcd_lvgl_create_label(s_dashboard_root, s_gas_text, value_color, 153);
    s_air_label = lcd_lvgl_create_label(s_dashboard_root, s_air_text, section_color, 182);
    s_network_label = lcd_lvgl_create_label(s_dashboard_root, s_network_text, section_color,
                                            LCD_DASHBOARD_CONNECTION_LABEL_Y);
    s_voice_label = lcd_lvgl_create_label(s_dashboard_root, s_voice_text, title_color, 0);
    if (title == NULL || environment == NULL || s_temp_label == NULL || s_humidity_label == NULL ||
        s_pressure_label == NULL || s_gas_label == NULL || s_air_label == NULL ||
        s_network_label == NULL || s_voice_label == NULL) {
        lvgl_port_unlock();
        return ESP_ERR_NO_MEM;
    }
    s_air_status_dot = lcd_lvgl_create_status_dot(s_dashboard_root,
                                                   LCD_DASHBOARD_STATUS_DOT_X,
                                                   LCD_DASHBOARD_AIR_DOT_Y);
    s_connection_status_dot = lcd_lvgl_create_status_dot(s_dashboard_root,
                                                          LCD_DASHBOARD_CONNECTION_DOT_X,
                                                          LCD_DASHBOARD_CONNECTION_DOT_Y);
    if (s_air_status_dot == NULL || s_connection_status_dot == NULL) {
        lvgl_port_unlock();
        return ESP_ERR_NO_MEM;
    }
    lv_obj_set_pos(s_network_label,
                   LCD_DASHBOARD_CONNECTION_LABEL_X,
                   LCD_DASHBOARD_CONNECTION_LABEL_Y);
    lv_obj_set_width(s_network_label, LCD_DASHBOARD_CONNECTION_LABEL_WIDTH);
    lv_obj_set_height(s_network_label, 18);
    lv_obj_set_width(s_temp_label, LCD_DASHBOARD_ENV_COLUMN_WIDTH);
    lv_obj_set_width(s_humidity_label, LCD_DASHBOARD_ENV_COLUMN_WIDTH);
    lv_obj_set_width(s_pressure_label, LCD_DASHBOARD_ENV_COLUMN_WIDTH);
    lv_obj_set_width(s_gas_label, LCD_DASHBOARD_ENV_COLUMN_WIDTH);
    lv_obj_set_width(s_air_label, LCD_DASHBOARD_ENV_COLUMN_WIDTH);
    lv_obj_set_pos(s_voice_label,
                   LCD_CAT_IMAGE_X,
                   LCD_CAT_IMAGE_Y + LCD_CAT_IMAGE_HEIGHT + 2);
    lv_obj_set_width(s_voice_label, LCD_CAT_IMAGE_WIDTH);
    lv_obj_set_style_text_align(s_voice_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

    lcd_lvgl_log_memory("before_cat_create");
    (void)lcd_cat_create(s_dashboard_root);
    lcd_lvgl_log_memory("after_cat_create");
    lcd_lvgl_log_memory("before_voice_ui_create");
    (void)lcd_voice_ui_create(screen);
    lcd_lvgl_log_memory("after_voice_ui_create");

    lcd_dashboard_snapshot_t initial_snapshot = {
        .voice_state = LCD_DASHBOARD_VOICE_LISTEN,
    };
    lcd_lvgl_update_dashboard(&initial_snapshot, true);

    lcd_lvgl_log_memory("before_first_refresh");
    lcd_lvgl_log_memory("before_first_voice_refresh");
    /* The display intentionally keeps a one-line partial DMA buffer. Invalidate the
     * complete screen once after the layout is built, then render it synchronously so
     * the panel's old PRESENCE area is overwritten by the background immediately. */
    lv_obj_invalidate(screen);
    lv_refr_now(s_display);
    lcd_lvgl_log_memory("after_first_refresh");
    lcd_lvgl_log_memory("after_first_voice_refresh");

    s_dashboard_refresh_timer = lv_timer_create(lcd_lvgl_dashboard_timer_cb,
                                                LCD_DASHBOARD_SNAPSHOT_REFRESH_MS,
                                                NULL);
    if (s_dashboard_refresh_timer == NULL) {
        ESP_LOGE(TAG, "dashboard timer creation failed");
        lvgl_port_unlock();
        return ESP_ERR_NO_MEM;
    }

    if (lv_timer_create(lcd_lvgl_memory_timer_cb, LCD_LVGL_MEM_LOG_PERIOD_MS, NULL) == NULL) {
        ESP_LOGE(MEM_TAG, "periodic timer creation failed");
        lvgl_port_unlock();
        return ESP_ERR_NO_MEM;
    }

    lcd_lvgl_log_memory("initial_ui_ready");
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

    lcd_lvgl_log_heap("legacy_buffer_release_before");
    ret = lcd_release_legacy_draw_buffer();
    if (ret != ESP_OK) {
        return ret;
    }
    lcd_lvgl_log_heap("legacy_buffer_release_after");

    const lvgl_port_cfg_t lvgl_cfg = {
        .task_priority = LCD_LVGL_TASK_PRIORITY,
        .task_stack = LCD_LVGL_TASK_STACK,
        .task_affinity = -1,
        .task_max_sleep_ms = 1000,
        .task_stack_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT,
        .timer_period_ms = LCD_LVGL_TIMER_PERIOD_MS,
    };
    lcd_lvgl_log_heap("lvgl_port_init_before");
    ret = lvgl_port_init(&lvgl_cfg);
    lcd_lvgl_log_heap("lvgl_port_init_after");
    if (ret != ESP_OK) {
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
    lcd_lvgl_log_heap("lvgl_display_created");
    ESP_LOGI(TAG, "LVGL display created: buffer_pixels=%u buffer_bytes=%u",
             (unsigned int)LCD_LVGL_DRAW_BUFFER_PIXELS,
             (unsigned int)(LCD_LVGL_DRAW_BUFFER_PIXELS * sizeof(uint16_t)));

    ret = lcd_lvgl_create_status_ui();
    if (ret != ESP_OK) {
        (void)lvgl_port_remove_disp(s_display);
        s_display = NULL;
        (void)lvgl_port_deinit();
        return ret;
    }

    boot_screen_set_dashboard_refresh_timer(s_dashboard_refresh_timer);
    boot_screen_start();

    lcd_lvgl_log_heap("display_on_before");
    if (!lvgl_port_lock(1000)) {
        (void)lvgl_port_remove_disp(s_display);
        s_display = NULL;
        (void)lvgl_port_deinit();
        return ESP_ERR_TIMEOUT;
    }
    ret = lcd_set_display_enabled(true);
    lvgl_port_unlock();
    lcd_lvgl_log_heap("display_on_after");
    if (ret != ESP_OK) {
        (void)lcd_set_display_enabled(false);
        (void)lvgl_port_remove_disp(s_display);
        s_display = NULL;
        (void)lvgl_port_deinit();
        return ret;
    }

    s_started = true;
    ESP_LOGI(TAG, "started with %u-byte single DMA draw buffer",
             (unsigned int)(LCD_LVGL_DRAW_BUFFER_PIXELS * sizeof(uint16_t)));

    ret = cst816t_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "LCD touch init failed: %s", esp_err_to_name(ret));
    } else {
        ret = lcd_lvgl_register_touch_indev();
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "LCD touch indev registration failed: %s", esp_err_to_name(ret));
        }
    }

    return ESP_OK;
}

bool lcd_service_is_started(void)
{
    return s_started;
}

void lcd_service_set_snapshot_provider(lcd_dashboard_snapshot_provider_t provider, void *user_ctx)
{
    portENTER_CRITICAL(&s_snapshot_provider_lock);
    s_snapshot_provider = provider;
    s_snapshot_provider_ctx = user_ctx;
    portEXIT_CRITICAL(&s_snapshot_provider_lock);
}
