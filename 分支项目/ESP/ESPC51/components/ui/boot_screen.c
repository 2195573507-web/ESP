#include "boot_screen.h"
#include "boot_cat_image.h"

#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

static const char *TAG = "BOOT_SCREEN";

/* WDT isolation: build only the boot root to isolate child-object failures. */
#define BOOT_SCREEN_ROOT_ONLY_TEST 0
#define BOOT_SCREEN_MINIMAL_TEST 0
#define BOOT_SCREEN_STATUS_ONLY_TEST 1
#define BOOT_SCREEN_DOTS_ONLY_TEST 0
#define BOOT_SCREEN_CAT_ENABLED 1
#define BOOT_SCREEN_STATUS_TEXT_ENABLED 1
#define BOOT_SCREEN_STATUS_DOTS_ENABLED 1
#define BOOT_SCREEN_STATUS_PROGRESS_ENABLED 1
#define BOOT_SCREEN_MIN_DURATION_MS 3000U
#define BOOT_SCREEN_MAX_DURATION_MS 8000U
#define BOOT_SCREEN_STAGE_CHECK_PERIOD_MS 100U

/* Keep ui independent from the Middlewares component graph. This read-only
 * service query only reads the BME initialization state. */
extern bool bme_sensor_service_is_initialized(void);
extern bool lcd_service_is_started(void);
extern bool wifi_is_connected(void);
extern bool audio_player_is_initialized(void);

static lv_obj_t *boot_screen_root = NULL;
static lv_obj_t *s_boot_dot;
static lv_obj_t *s_boot_title;
static lv_obj_t *s_boot_model;
static lv_obj_t *s_boot_cat;
static lv_obj_t *s_boot_status;
static lv_obj_t *s_boot_progress;
static lv_obj_t *s_boot_display_label;
static lv_obj_t *s_boot_sensor_label;
static lv_obj_t *s_boot_network_label;
static lv_obj_t *s_boot_audio_label;
static lv_obj_t *s_boot_display_dot;
static lv_obj_t *s_boot_sensor_dot;
static lv_obj_t *s_boot_network_dot;
static lv_obj_t *s_boot_audio_dot;
static lv_timer_t *s_boot_cleanup_timer;
static lv_timer_t *s_boot_stage_timer;
static lv_timer_t *s_boot_progress_timer;
static lv_timer_t *s_boot_cat_timer;
static lv_timer_t *s_dashboard_refresh_timer;
static uint8_t s_boot_progress_value;
static bool s_boot_cat_at_rest;
static bool s_boot_display_ready;
static bool s_boot_sensor_ready;
static bool s_boot_network_ready;
static bool s_boot_audio_ready;
static uint32_t s_boot_started_at_ms;

/*
 * The boot UI is an overlay so the existing Dashboard and voice pages are
 * created exactly once and are revealed unchanged when this root is deleted.
 * The Dashboard cat is reused by reading its existing LVGL image source, so
 * boot UI does not duplicate or own any pixel resource.
 */

/* Stage 1: fade in a fixed-size center dot during the first 500 ms. */
static void boot_screen_set_opa(void *obj, int32_t value)
{
    lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)value, LV_PART_MAIN);
}

static void boot_screen_show_object(void *obj, int32_t value)
{
    if (value != 0) {
        lv_obj_clear_flag((lv_obj_t *)obj, LV_OBJ_FLAG_HIDDEN);
    }
}

static void boot_screen_hide_object(void *obj, int32_t value)
{
    if (value != 0) {
        lv_obj_add_flag((lv_obj_t *)obj, LV_OBJ_FLAG_HIDDEN);
    }
}

static void boot_screen_schedule_visibility(lv_obj_t *obj, bool show, uint32_t delay_ms)
{
    lv_anim_t anim;
    lv_anim_init(&anim);
    lv_anim_set_var(&anim, obj);
    lv_anim_set_exec_cb(&anim, show ? boot_screen_show_object : boot_screen_hide_object);
    lv_anim_set_values(&anim, 0, 1);
    lv_anim_set_duration(&anim, 1);
    lv_anim_set_delay(&anim, delay_ms);
    lv_anim_start(&anim);
}

static void boot_screen_clear_object_refs(void)
{
    s_boot_dot = NULL;
    s_boot_title = NULL;
    s_boot_model = NULL;
    s_boot_cat = NULL;
    s_boot_status = NULL;
    s_boot_progress = NULL;
    s_boot_display_label = NULL;
    s_boot_sensor_label = NULL;
    s_boot_network_label = NULL;
    s_boot_audio_label = NULL;
    s_boot_display_dot = NULL;
    s_boot_sensor_dot = NULL;
    s_boot_network_dot = NULL;
    s_boot_audio_dot = NULL;
    s_boot_display_ready = false;
    s_boot_sensor_ready = false;
    s_boot_network_ready = false;
    s_boot_audio_ready = false;
}

static void boot_screen_stop_animations(void)
{
    if (s_boot_dot != NULL) {
        (void)lv_anim_delete(s_boot_dot, NULL);
    }
    if (s_boot_title != NULL) {
        (void)lv_anim_delete(s_boot_title, NULL);
    }
    if (s_boot_model != NULL) {
        (void)lv_anim_delete(s_boot_model, NULL);
    }
    if (s_boot_cat != NULL) {
        (void)lv_anim_delete(s_boot_cat, NULL);
    }
    if (s_boot_status != NULL) {
        (void)lv_anim_delete(s_boot_status, NULL);
    }
}

void boot_screen_set_dashboard_refresh_timer(lv_timer_t *timer)
{
    s_dashboard_refresh_timer = timer;
}

static void boot_screen_cleanup_timer_cb(lv_timer_t *timer)
{
    if (timer == s_boot_cleanup_timer) {
        s_boot_cleanup_timer = NULL;
    }

    if (s_boot_progress_timer != NULL) {
        lv_timer_pause(s_boot_progress_timer);
        lv_timer_delete(s_boot_progress_timer);
        s_boot_progress_timer = NULL;
    }
    if (s_boot_cat_timer != NULL) {
        lv_timer_pause(s_boot_cat_timer);
        lv_timer_delete(s_boot_cat_timer);
        s_boot_cat_timer = NULL;
    }

    boot_screen_stop_animations();
    if (boot_screen_root != NULL) {
        lv_obj_delete(boot_screen_root);
        boot_screen_root = NULL;
    }
    boot_screen_clear_object_refs();
    if (s_dashboard_refresh_timer != NULL) {
        lv_timer_resume(s_dashboard_refresh_timer);
    }
    ESP_LOGI(TAG, "boot screen finished");

    if (timer != NULL) {
        lv_timer_delete(timer);
    }
}

static void boot_screen_finish_cb(lv_anim_t *anim)
{
    (void)anim;
    if (boot_screen_root == NULL || s_boot_cleanup_timer != NULL) {
        return;
    }

    s_boot_cleanup_timer = lv_timer_create(boot_screen_cleanup_timer_cb, 350, NULL);
    if (s_boot_cleanup_timer == NULL) {
        ESP_LOGW(TAG, "boot screen cleanup timer allocation failed");
        return;
    }
    lv_timer_set_repeat_count(s_boot_cleanup_timer, 1);
    lv_timer_set_auto_delete(s_boot_cleanup_timer, false);
}

static void boot_screen_stage_timer_cb(lv_timer_t *timer)
{
    if (timer == NULL || timer != s_boot_stage_timer || boot_screen_root == NULL) {
        return;
    }

    const bool display_ready = lcd_service_is_started();
    const bool sensor_ready = bme_sensor_service_is_initialized();
    const bool network_ready = wifi_is_connected();
    const bool audio_ready = audio_player_is_initialized();
    const lv_color_t ready_color = lv_color_hex(0x79D2A6);
    const lv_color_t init_color = lv_color_hex(0xE5B84B);

#define BOOT_SCREEN_UPDATE_STATUS(name, title) \
    do { \
        if (name##_ready != s_boot_##name##_ready) { \
            const lv_color_t color = name##_ready ? ready_color : init_color; \
            s_boot_##name##_ready = name##_ready; \
            if (s_boot_##name##_label != NULL) { \
                lv_label_set_text(s_boot_##name##_label, \
                                  name##_ready ? title " READY" : title " INIT"); \
                lv_obj_set_style_text_color(s_boot_##name##_label, color, LV_PART_MAIN); \
            } \
            if (s_boot_##name##_dot != NULL) { \
                lv_obj_set_style_bg_color(s_boot_##name##_dot, color, LV_PART_MAIN); \
            } \
        } \
    } while (0)

    BOOT_SCREEN_UPDATE_STATUS(display, "Display");
    BOOT_SCREEN_UPDATE_STATUS(sensor, "Sensor");
    BOOT_SCREEN_UPDATE_STATUS(network, "Network");
    BOOT_SCREEN_UPDATE_STATUS(audio, "Audio");
#undef BOOT_SCREEN_UPDATE_STATUS

    const uint32_t elapsed_ms = lv_tick_elaps(s_boot_started_at_ms);
    if ((elapsed_ms >= BOOT_SCREEN_MIN_DURATION_MS && display_ready && sensor_ready &&
         network_ready && audio_ready) ||
        elapsed_ms >= BOOT_SCREEN_MAX_DURATION_MS) {
        s_boot_stage_timer = NULL;
        boot_screen_finish_cb(NULL);
        lv_timer_delete(timer);
    }
}

static void boot_screen_schedule_finish_timer(void)
{
    s_boot_stage_timer = lv_timer_create(boot_screen_stage_timer_cb,
                                         BOOT_SCREEN_STAGE_CHECK_PERIOD_MS,
                                         NULL);
    if (s_boot_stage_timer != NULL) {
        lv_timer_set_repeat_count(s_boot_stage_timer, -1);
        lv_timer_set_auto_delete(s_boot_stage_timer, false);
    }
}

static void boot_screen_progress_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    s_boot_progress_value += 4U;
    lv_bar_set_value(s_boot_progress, s_boot_progress_value, LV_ANIM_OFF);
}

static void boot_screen_start_progress_timer(void)
{
    if (s_boot_progress == NULL) {
        return;
    }

    s_boot_progress_value = 0;
    lv_bar_set_value(s_boot_progress, s_boot_progress_value, LV_ANIM_OFF);
    s_boot_progress_timer = lv_timer_create(boot_screen_progress_timer_cb, 100, NULL);
    if (s_boot_progress_timer != NULL) {
        lv_timer_set_repeat_count(s_boot_progress_timer, 25);
        lv_timer_set_auto_delete(s_boot_progress_timer, false);
    }
}

static void boot_screen_cat_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (s_boot_cat == NULL) {
        return;
    }

    s_boot_cat_at_rest = !s_boot_cat_at_rest;
    lv_obj_set_y(s_boot_cat, s_boot_cat_at_rest ? 48 : 51);
}

static void boot_screen_start_cat_timer(void)
{
    if (s_boot_cat == NULL) {
        return;
    }

    s_boot_cat_at_rest = true;
    s_boot_cat_timer = lv_timer_create(boot_screen_cat_timer_cb, 300, NULL);
}

static lv_obj_t *boot_screen_create_label(lv_obj_t *parent,
                                          const char *text,
                                          int16_t x,
                                          int16_t y,
                                          int16_t width,
                                          lv_color_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    if (label == NULL) {
        return NULL;
    }
    lv_label_set_text_static(label, text);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_width(label, width);
    lv_obj_set_style_text_color(label, color, LV_PART_MAIN);
    return label;
}

static lv_obj_t *create_boot_dot(lv_obj_t *root,
                                 int16_t x,
                                 int16_t y,
                                 lv_color_t color)
{
    lv_obj_t *dot = lv_obj_create(root);
    if (dot == NULL) {
        return NULL;
    }
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, 6, 6);
    lv_obj_set_pos(dot, x, y);
    lv_obj_set_style_bg_color(dot, color, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(dot, 3, LV_PART_MAIN);
    return dot;
}

#if BOOT_SCREEN_STATUS_TEXT_ENABLED
static void boot_screen_create_status_row(lv_obj_t *parent,
                                          const char *text,
                                          int16_t y,
                                          int16_t dot_x,
                                          int16_t dot_y,
                                          lv_color_t dot_color,
                                          lv_color_t text_color)
{
#if BOOT_SCREEN_STATUS_DOTS_ENABLED
    lv_obj_t *dot = lv_obj_create(boot_screen_root);
    if (dot != NULL) {
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, 6, 6);
        lv_obj_set_pos(dot, dot_x, dot_y);
        lv_obj_set_style_radius(dot, 3, LV_PART_MAIN);
        lv_obj_set_style_bg_color(dot, dot_color, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, LV_PART_MAIN);
    }
#endif
    (void)boot_screen_create_label(parent, text, 38, y, 120, text_color);
}

#endif

void boot_screen_start(void)
{
    lv_display_t *display = lv_display_get_default();
    if (display == NULL || !lvgl_port_lock(1000)) {
        ESP_LOGW(TAG, "boot screen skipped: LVGL display unavailable");
        return;
    }

    if (boot_screen_root != NULL || s_boot_cleanup_timer != NULL) {
        lvgl_port_unlock();
        return;
    }

    if (s_dashboard_refresh_timer != NULL) {
        lv_timer_pause(s_dashboard_refresh_timer);
    }

    lv_obj_t *screen = lv_screen_active();
    boot_screen_root = lv_obj_create(screen);
    if (boot_screen_root == NULL) {
        if (s_dashboard_refresh_timer != NULL) {
            lv_timer_resume(s_dashboard_refresh_timer);
        }
        lvgl_port_unlock();
        ESP_LOGW(TAG, "boot screen skipped: root allocation failed");
        return;
    }
    lv_obj_remove_style_all(boot_screen_root);
    lv_obj_set_pos(boot_screen_root, 0, 0);
    lv_obj_set_size(boot_screen_root, 240, 284);
    lv_obj_set_style_bg_color(boot_screen_root, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(boot_screen_root, LV_OPA_COVER, LV_PART_MAIN);
    s_boot_started_at_ms = lv_tick_get();

#if BOOT_SCREEN_ROOT_ONLY_TEST
    /* Reuse the normal deferred cleanup path without creating a child object. */
    lv_anim_t isolated_finish;
    lv_anim_init(&isolated_finish);
    lv_anim_set_var(&isolated_finish, boot_screen_root);
    lv_anim_set_exec_cb(&isolated_finish, boot_screen_set_opa);
    lv_anim_set_values(&isolated_finish, LV_OPA_COVER, LV_OPA_COVER);
    lv_anim_set_duration(&isolated_finish, 1);
    lv_anim_set_delay(&isolated_finish, 3000);
    lv_anim_set_completed_cb(&isolated_finish, boot_screen_finish_cb);
    lv_anim_start(&isolated_finish);
#elif BOOT_SCREEN_MINIMAL_TEST
    const lv_color_t accent = lv_color_hex(0x79D2A6);
    s_boot_title = lv_label_create(boot_screen_root);
    if (s_boot_title != NULL) {
        lv_label_set_text(s_boot_title, "SYSTEM CHECK");
        lv_obj_set_pos(s_boot_title, 0, 22);
        lv_obj_set_width(s_boot_title, 240);
        lv_obj_set_style_text_align(s_boot_title, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_set_style_text_color(s_boot_title, accent, LV_PART_MAIN);
    }

    s_boot_dot = lv_obj_create(boot_screen_root);
    if (s_boot_dot != NULL) {
        lv_obj_remove_style_all(s_boot_dot);
        lv_obj_set_size(s_boot_dot, 6, 6);
        lv_obj_set_pos(s_boot_dot, 54, 174);
        lv_obj_set_style_bg_color(s_boot_dot, accent, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(s_boot_dot, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_radius(s_boot_dot, 3, LV_PART_MAIN);
    }

    lv_anim_t isolated_finish;
    lv_anim_init(&isolated_finish);
    lv_anim_set_var(&isolated_finish, boot_screen_root);
    lv_anim_set_exec_cb(&isolated_finish, boot_screen_set_opa);
    lv_anim_set_values(&isolated_finish, LV_OPA_COVER, LV_OPA_COVER);
    lv_anim_set_duration(&isolated_finish, 1);
    lv_anim_set_delay(&isolated_finish, 3000);
    lv_anim_set_completed_cb(&isolated_finish, boot_screen_finish_cb);
    lv_anim_start(&isolated_finish);
#elif BOOT_SCREEN_STATUS_ONLY_TEST
    const lv_color_t init_color = lv_color_hex(0xE5B84B);
    const char *status_text[] = {"Display INIT", "Sensor INIT", "Network INIT", "Audio INIT"};
    const int16_t status_y[] = {125, 150, 175, 200};
    for (size_t i = 0; i < 4; i++) {
        lv_obj_t *label = lv_label_create(boot_screen_root);
        if (label != NULL) {
            lv_label_set_text(label, status_text[i]);
            lv_obj_set_pos(label, 70, status_y[i]);
            lv_obj_set_style_text_color(label, init_color, LV_PART_MAIN);
            if (i == 0U) {
                s_boot_display_label = label;
            } else if (i == 1U) {
                s_boot_sensor_label = label;
            } else if (i == 2U) {
                s_boot_network_label = label;
            } else {
                s_boot_audio_label = label;
            }
        }
    }
    s_boot_display_dot = create_boot_dot(boot_screen_root, 50, 125, init_color);
    s_boot_sensor_dot = create_boot_dot(boot_screen_root, 50, 150, init_color);
    s_boot_network_dot = create_boot_dot(boot_screen_root, 50, 175, init_color);
    s_boot_audio_dot = create_boot_dot(boot_screen_root, 50, 200, init_color);
#if BOOT_SCREEN_CAT_ENABLED
    s_boot_cat = lv_image_create(boot_screen_root);
    if (s_boot_cat != NULL) {
        lv_image_set_src(s_boot_cat, &boot_cat_image);
        lv_obj_set_pos(s_boot_cat, (240 - BOOT_CAT_IMAGE_WIDTH) / 2, 48);
        boot_screen_start_cat_timer();
    }
#endif
#if BOOT_SCREEN_STATUS_PROGRESS_ENABLED
    s_boot_progress = lv_bar_create(boot_screen_root);
    if (s_boot_progress != NULL) {
        lv_obj_set_pos(s_boot_progress, 40, 240);
        lv_obj_set_size(s_boot_progress, 160, 8);
        boot_screen_start_progress_timer();
    }
#endif
    boot_screen_schedule_finish_timer();
#elif BOOT_SCREEN_DOTS_ONLY_TEST
    const lv_color_t ready_color = lv_color_hex(0x79D2A6);
    const lv_color_t init_color = lv_color_hex(0xE5B84B);
    (void)create_boot_dot(boot_screen_root, 54, 174, ready_color);
    (void)create_boot_dot(boot_screen_root, 54, 193, ready_color);
    (void)create_boot_dot(boot_screen_root, 54, 212, init_color);
    (void)create_boot_dot(boot_screen_root, 54, 231, init_color);

    boot_screen_schedule_finish_timer();
#else
    const lv_color_t accent = lv_color_hex(0x79D2A6);
    const lv_color_t text_color = lv_color_hex(0xDCE7EF);
    s_boot_dot = lv_obj_create(boot_screen_root);
    if (s_boot_dot != NULL) {
        lv_obj_remove_style_all(s_boot_dot);
        lv_obj_set_size(s_boot_dot, 24, 24);
        lv_obj_align(s_boot_dot, LV_ALIGN_CENTER, 0, -18);
        lv_obj_set_style_radius(s_boot_dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_boot_dot, accent, LV_PART_MAIN);
        lv_obj_set_style_bg_opa(s_boot_dot, LV_OPA_TRANSP, LV_PART_MAIN);

        lv_anim_t dot_opa;
        lv_anim_init(&dot_opa);
        lv_anim_set_var(&dot_opa, s_boot_dot);
        lv_anim_set_exec_cb(&dot_opa, boot_screen_set_opa);
        lv_anim_set_values(&dot_opa, LV_OPA_TRANSP, LV_OPA_COVER);
        lv_anim_set_duration(&dot_opa, 500);
        lv_anim_start(&dot_opa);
        boot_screen_schedule_visibility(s_boot_dot, false, 500);
    }

    boot_screen_schedule_finish_timer();

    /* Stage 2: compact product title and the existing Dashboard cat image. */
    s_boot_title = boot_screen_create_label(boot_screen_root,
                                            "SensAir",
                                            0,
                                            22,
                                            240,
                                            text_color);
    if (s_boot_title != NULL) {
        lv_obj_set_style_text_align(s_boot_title, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_add_flag(s_boot_title, LV_OBJ_FLAG_HIDDEN);
        boot_screen_schedule_visibility(s_boot_title, true, 500);
    }

    s_boot_model = boot_screen_create_label(boot_screen_root, "C5", 0, 46, 240, accent);
    if (s_boot_model != NULL) {
        lv_obj_set_style_text_align(s_boot_model, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        lv_obj_add_flag(s_boot_model, LV_OBJ_FLAG_HIDDEN);
        boot_screen_schedule_visibility(s_boot_model, true, 500);
    }

#if BOOT_SCREEN_CAT_ENABLED
    s_boot_cat = lv_image_create(boot_screen_root);
    if (s_boot_cat != NULL) {
        lv_image_set_src(s_boot_cat, &boot_cat_image);
        lv_obj_set_pos(s_boot_cat, (240 - BOOT_CAT_IMAGE_WIDTH) / 2, 35);
        lv_obj_add_flag(s_boot_cat, LV_OBJ_FLAG_HIDDEN);
        boot_screen_schedule_visibility(s_boot_cat, true, 500);
        boot_screen_start_cat_timer();
    }
#endif

#if BOOT_SCREEN_STATUS_TEXT_ENABLED
    const lv_color_t init_color = lv_color_hex(0xE5B84B);
    /* Stage 3: a compact status block; all children are owned by the boot root. */
    s_boot_status = lv_obj_create(boot_screen_root);
    if (s_boot_status != NULL) {
        lv_obj_remove_style_all(s_boot_status);
        lv_obj_set_pos(s_boot_status, 30, 145);
        lv_obj_set_size(s_boot_status, 180, 119);
        lv_obj_set_style_bg_opa(s_boot_status, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_add_flag(s_boot_status, LV_OBJ_FLAG_HIDDEN);

        lv_obj_t *heading = boot_screen_create_label(s_boot_status,
                                                      "SYSTEM CHECK",
                                                      0,
                                                      0,
                                                      180,
                                                      accent);
        if (heading != NULL) {
            lv_obj_set_style_text_align(heading, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
        }
        boot_screen_create_status_row(s_boot_status, "Display", 24, 54, 174, accent, text_color);
        boot_screen_create_status_row(s_boot_status, "Sensor", 43, 54, 193, accent, text_color);
        boot_screen_create_status_row(s_boot_status, "Network", 62, 54, 212, init_color, text_color);
        boot_screen_create_status_row(s_boot_status, "Audio", 81, 54, 231, init_color, text_color);

#if BOOT_SCREEN_STATUS_PROGRESS_ENABLED
        s_boot_progress = lv_bar_create(s_boot_status);
        if (s_boot_progress != NULL) {
            lv_obj_set_pos(s_boot_progress, 24, 103);
            lv_obj_set_size(s_boot_progress, 132, 8);
            lv_obj_set_style_bg_color(s_boot_progress, lv_color_hex(0x26323A), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(s_boot_progress, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_set_style_bg_color(s_boot_progress, accent, LV_PART_INDICATOR);
            lv_obj_set_style_bg_opa(s_boot_progress, LV_OPA_COVER, LV_PART_INDICATOR);
            lv_obj_set_style_radius(s_boot_progress, 3, LV_PART_MAIN);
            lv_obj_set_style_radius(s_boot_progress, 3, LV_PART_INDICATOR);
            lv_bar_set_range(s_boot_progress, 0, 100);
            boot_screen_start_progress_timer();
        }
#endif
        boot_screen_schedule_visibility(s_boot_status, true, 1500);
    }
#endif

    lv_obj_invalidate(boot_screen_root);
#endif
    ESP_LOGI(TAG,
             "boot screen started min_duration_ms=%u max_wait_ms=%u",
             (unsigned int)BOOT_SCREEN_MIN_DURATION_MS,
             (unsigned int)BOOT_SCREEN_MAX_DURATION_MS);
    lvgl_port_unlock();
}
