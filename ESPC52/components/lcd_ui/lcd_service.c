#include "lcd_service.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "lcd_driver.h"
#include "lcd_board_profile.h"
#include "lcd_fault_injection.h"
#include "lcd_ui.h"

#define LCD_SERVICE_CONTEXT_BYTES 2048U
#define LCD_SERVICE_TICK_MS 100U
#define LCD_SERVICE_SNAPSHOT_MS 1000U
#define LCD_SERVICE_DEFAULT_TTL_MS 5000U

typedef enum {
    LCD_SERVICE_UNINITIALIZED = 0,
    LCD_SERVICE_ALLOCATING,
    LCD_SERVICE_PUBLISHED,
    LCD_SERVICE_STOPPING,
    LCD_SERVICE_FAILED,
} lcd_service_state_t;

typedef struct {
    lcd_service_state_t state;
    QueueHandle_t wake_queue;
    lv_timer_t *timer;
    lcd_system_snapshot_t snapshot;
    lcd_command_t command;
    uint64_t command_expiry_ms;
    uint64_t last_full_refresh_ms;
} lcd_service_context_t;

static const char *TAG = "lcd_service";
static StaticSemaphore_t s_lock_storage;
static SemaphoreHandle_t s_lock;
static StaticQueue_t s_wake_queue_storage;
static uint8_t s_wake_queue_buffer[sizeof(lcd_wake_event_t)];
static QueueHandle_t s_wake_queue;
static portMUX_TYPE s_data_lock = portMUX_INITIALIZER_UNLOCKED;
static lcd_service_context_t *s_ctx;
/* Prevent a new start from reusing driver state while a detached context unwinds. */
static bool s_stop_in_progress;

_Static_assert(sizeof(lcd_service_context_t) <= LCD_SERVICE_CONTEXT_BYTES,
               "service context must fit in its fixed PSRAM allocation");

static esp_err_t lcd_service_lock(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutexStatic(&s_lock_storage);
    }
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    return xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

static void lcd_service_unlock(void)
{
    (void)xSemaphoreGive(s_lock);
}

static uint64_t lcd_service_now_ms(void)
{
    return (uint64_t)esp_timer_get_time() / 1000U;
}

static void lcd_service_copy_bounded(char *destination, size_t destination_size, const char *source)
{
    if (destination == NULL || destination_size == 0U) {
        return;
    }
    size_t length = 0U;
    if (source != NULL) {
        while (length + 1U < destination_size && source[length] != '\0') {
            ++length;
        }
        memcpy(destination, source, length);
    }
    destination[length] = '\0';
}

static esp_err_t lcd_service_wake_request(void *user_ctx)
{
    (void)user_ctx;
    return lcd_service_request_wake_event();
}

static void lcd_service_timer_cb(lv_timer_t *timer)
{
    lcd_service_context_t *const ctx = timer != NULL ? lv_timer_get_user_data(timer) : NULL;
    if (ctx == NULL) {
        return;
    }
    const uint64_t now_ms = lcd_service_now_ms();
    lcd_system_snapshot_t snapshot;
    lcd_command_t command;
    bool command_visible;
    portENTER_CRITICAL(&s_data_lock);
    if (ctx != s_ctx || ctx->state != LCD_SERVICE_PUBLISHED) {
        portEXIT_CRITICAL(&s_data_lock);
        return;
    }
    snapshot = ctx->snapshot;
    command = ctx->command;
    command_visible = ctx->command_expiry_ms > now_ms;
    portEXIT_CRITICAL(&s_data_lock);
    const bool full_refresh = now_ms - ctx->last_full_refresh_ms >= LCD_SERVICE_SNAPSHOT_MS;
    if (full_refresh) {
        ctx->last_full_refresh_ms = now_ms;
    }
    lcd_ui_apply(&snapshot, &command, command_visible, full_refresh);
}

static esp_err_t lcd_service_cleanup(lcd_service_context_t *ctx)
{
    if (ctx == NULL) {
        return ESP_OK;
    }
    if (lcd_driver_get_display() != NULL) {
        if (!lvgl_port_lock(1000U)) {
            /* Do not free ctx: an LVGL timer can still retain user_data. */
            return ESP_ERR_TIMEOUT;
        }
        if (ctx->timer != NULL) {
            lv_timer_delete(ctx->timer);
            ctx->timer = NULL;
        }
        (void)lcd_ui_stop();
        lvgl_port_unlock();
    }
    (void)lcd_driver_stop();
    if (ctx->wake_queue != NULL && ctx->wake_queue != s_wake_queue) {
        vQueueDelete(ctx->wake_queue);
    }
    ctx->wake_queue = NULL;
    heap_caps_free(ctx);
    return ESP_OK;
}

/* Called with s_lock held. A lock-timeout cleanup leaves callback-bearing state
 * intact, so retain it as STOPPING and require the caller to retry stop. */
static esp_err_t lcd_service_cleanup_after_start_failure(lcd_service_context_t *ctx)
{
    const esp_err_t cleanup_ret = lcd_service_cleanup(ctx);
    if (cleanup_ret != ESP_OK) {
        portENTER_CRITICAL(&s_data_lock);
        ctx->state = LCD_SERVICE_STOPPING;
        s_ctx = ctx;
        portEXIT_CRITICAL(&s_data_lock);
    }
    return cleanup_ret;
}

esp_err_t lcd_service_start(void)
{
    const esp_err_t lock_ret = lcd_service_lock();
    if (lock_ret != ESP_OK) {
        return lock_ret;
    }
    if (s_ctx != NULL && s_ctx->state == LCD_SERVICE_PUBLISHED) {
        lcd_service_unlock();
        return ESP_OK;
    }
    if (s_ctx != NULL || s_stop_in_progress) {
        lcd_service_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t ret = lcd_driver_start();
    if (ret != ESP_OK) {
        lcd_service_unlock();
        return ret;
    }
    ret = lcd_driver_register_lvgl(lcd_ui_prepare_lvgl_pool,
                                   lcd_ui_release_lvgl_pool,
                                   NULL);
    if (ret != ESP_OK) {
        (void)lcd_driver_stop();
        lcd_service_unlock();
        return ret;
    }

    const uint32_t psram_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    if (heap_caps_get_free_size(psram_caps) < LCD_SERVICE_CONTEXT_BYTES ||
        heap_caps_get_largest_free_block(psram_caps) < LCD_SERVICE_CONTEXT_BYTES) {
        (void)lcd_driver_stop();
        lcd_service_unlock();
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG,
             "MEM_ALLOC_PLAN owner=lcd_service_context caps=0x%08lx size=%u region=psram",
             (unsigned long)psram_caps,
             (unsigned int)LCD_SERVICE_CONTEXT_BYTES);
    lcd_service_context_t *ctx = lcd_fault_injection_should_fail(LCD_FAULT_SERVICE_CONTEXT) ? NULL : heap_caps_calloc(1, LCD_SERVICE_CONTEXT_BYTES, psram_caps);
    if (ctx == NULL) {
        (void)lcd_driver_stop();
        lcd_service_unlock();
        return ESP_ERR_NO_MEM;
    }
    ctx->state = LCD_SERVICE_ALLOCATING;
    const bool wake_queue_forced_failure = lcd_fault_injection_should_fail(LCD_FAULT_WAKE_QUEUE);
    if (s_wake_queue == NULL && !wake_queue_forced_failure) {
        s_wake_queue = xQueueCreateStatic(1U,
                                          sizeof(lcd_wake_event_t),
                                          s_wake_queue_buffer,
                                          &s_wake_queue_storage);
    }
    if (s_wake_queue != NULL) {
        (void)xQueueReset(s_wake_queue);
    }
    ctx->wake_queue = wake_queue_forced_failure ? NULL : s_wake_queue;
    if (ctx->wake_queue == NULL) {
        const esp_err_t cleanup_ret = lcd_service_cleanup_after_start_failure(ctx);
        lcd_service_unlock();
        return cleanup_ret != ESP_OK ? cleanup_ret : ESP_ERR_NO_MEM;
    }
    if (!lvgl_port_lock(1000U)) {
        const esp_err_t cleanup_ret = lcd_service_cleanup_after_start_failure(ctx);
        lcd_service_unlock();
        return cleanup_ret != ESP_OK ? cleanup_ret : ESP_ERR_TIMEOUT;
    }
    ret = lcd_ui_start(lcd_driver_get_display(), lcd_service_wake_request, NULL);
    lvgl_port_unlock();
    if (ret != ESP_OK) {
        const esp_err_t cleanup_ret = lcd_service_cleanup_after_start_failure(ctx);
        lcd_service_unlock();
        return cleanup_ret != ESP_OK ? cleanup_ret : ret;
    }
    if (!lvgl_port_lock(1000U)) {
        const esp_err_t cleanup_ret = lcd_service_cleanup_after_start_failure(ctx);
        lcd_service_unlock();
        return cleanup_ret != ESP_OK ? cleanup_ret : ESP_ERR_TIMEOUT;
    }
    ctx->timer = lcd_fault_injection_should_fail(LCD_FAULT_UI_TIMER) ? NULL : lv_timer_create(lcd_service_timer_cb, LCD_SERVICE_TICK_MS, ctx);
    lvgl_port_unlock();
    if (ctx->timer == NULL) {
        const esp_err_t cleanup_ret = lcd_service_cleanup_after_start_failure(ctx);
        lcd_service_unlock();
        return cleanup_ret != ESP_OK ? cleanup_ret : ESP_ERR_NO_MEM;
    }
    ctx->state = LCD_SERVICE_PUBLISHED;
    ctx->last_full_refresh_ms = 0U;
    s_ctx = ctx;
    ESP_LOGI(TAG, "LCD_SERVICE_PUBLISHED draw_buffer=%u legacy_released=1", (unsigned)LCD_LVGL_DRAW_BYTES);
    lcd_service_unlock();
    return ESP_OK;
}

esp_err_t lcd_service_stop(void)
{
    const esp_err_t lock_ret = lcd_service_lock();
    if (lock_ret != ESP_OK) {
        return lock_ret;
    }
    lcd_service_context_t *const ctx = s_ctx;
    if (ctx == NULL) {
        lcd_service_unlock();
        return ESP_OK;
    }
    if (s_stop_in_progress) {
        lcd_service_unlock();
        return ESP_OK;
    }
    portENTER_CRITICAL(&s_data_lock);
    ctx->state = LCD_SERVICE_STOPPING;
    s_ctx = NULL;
    portEXIT_CRITICAL(&s_data_lock);
    s_stop_in_progress = true;
    lcd_service_unlock();
    const esp_err_t ret = lcd_service_cleanup(ctx);
    const esp_err_t relock_ret = lcd_service_lock();
    if (relock_ret != ESP_OK) {
        portENTER_CRITICAL(&s_data_lock);
        s_stop_in_progress = false;
        if (ret != ESP_OK) {
            s_ctx = ctx;
        }
        portEXIT_CRITICAL(&s_data_lock);
        return ret != ESP_OK ? ret : relock_ret;
    }
    s_stop_in_progress = false;
    if (ret != ESP_OK) {
        /* The cleanup path left ctx allocated, so retain it for an explicit retry. */
        portENTER_CRITICAL(&s_data_lock);
        s_ctx = ctx;
        portEXIT_CRITICAL(&s_data_lock);
    }
    lcd_service_unlock();
    return ret;
}

bool lcd_service_is_started(void)
{
    if (lcd_service_lock() != ESP_OK) {
        return false;
    }
    const bool started = s_ctx != NULL && s_ctx->state == LCD_SERVICE_PUBLISHED;
    lcd_service_unlock();
    return started;
}

esp_err_t lcd_service_post_snapshot(const lcd_system_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t lock_ret = lcd_service_lock();
    if (lock_ret != ESP_OK) {
        return lock_ret;
    }
    lcd_service_context_t *const ctx = s_ctx;
    if (ctx == NULL || ctx->state != LCD_SERVICE_PUBLISHED) {
        lcd_service_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    portENTER_CRITICAL(&s_data_lock);
    ctx->snapshot = *snapshot;
    portEXIT_CRITICAL(&s_data_lock);
    lcd_service_unlock();
    return ESP_OK;
}

esp_err_t lcd_service_post_command(const lcd_command_t *command)
{
    if (command == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t lock_ret = lcd_service_lock();
    if (lock_ret != ESP_OK) {
        return lock_ret;
    }
    lcd_service_context_t *const ctx = s_ctx;
    if (ctx == NULL || ctx->state != LCD_SERVICE_PUBLISHED) {
        lcd_service_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    lcd_command_t bounded = *command;
    lcd_service_copy_bounded(bounded.title, sizeof(bounded.title), command->title);
    lcd_service_copy_bounded(bounded.text, sizeof(bounded.text), command->text);
    if (bounded.ttl_ms == 0U) {
        bounded.ttl_ms = LCD_SERVICE_DEFAULT_TTL_MS;
    }
    portENTER_CRITICAL(&s_data_lock);
    ctx->command = bounded;
    ctx->command_expiry_ms = lcd_service_now_ms() + bounded.ttl_ms;
    portEXIT_CRITICAL(&s_data_lock);
    lcd_service_unlock();
    return ESP_OK;
}

esp_err_t lcd_service_request_wake_event(void)
{
    const esp_err_t lock_ret = lcd_service_lock();
    if (lock_ret != ESP_OK) {
        return lock_ret;
    }
    lcd_service_context_t *const ctx = s_ctx;
    if (ctx == NULL || ctx->state != LCD_SERVICE_PUBLISHED || ctx->wake_queue == NULL) {
        lcd_service_unlock();
        return ESP_ERR_INVALID_STATE;
    }
    uint32_t generation = 0U;
    portENTER_CRITICAL(&s_data_lock);
    generation = ctx->snapshot.generation;
    portEXIT_CRITICAL(&s_data_lock);
    const lcd_wake_event_t event = {
        .generation = generation,
        .timestamp_ms = lcd_service_now_ms(),
    };
    if (xQueueOverwrite(ctx->wake_queue, &event) != pdPASS) {
        lcd_service_unlock();
        return ESP_ERR_TIMEOUT;
    }
    lcd_service_unlock();
    ESP_LOGI(TAG, "LCD_WAKE_EVENT_POSTED generation=%u", (unsigned)event.generation);
    return ESP_OK;
}

bool lcd_service_take_wake_event(lcd_wake_event_t *out_event)
{
    if (out_event == NULL || lcd_service_lock() != ESP_OK) {
        return false;
    }
    lcd_service_context_t *const ctx = s_ctx;
    const bool received = ctx != NULL && ctx->state == LCD_SERVICE_PUBLISHED &&
                          ctx->wake_queue != NULL &&
                          xQueueReceive(ctx->wake_queue, out_event, 0U) == pdPASS;
    lcd_service_unlock();
    return received;
}
