#include "lcd_driver.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_memory_utils.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "lcd_board_profile.h"
#include "lcd_fault_injection.h"

#define LCD_RUNTIME_DMA_LARGEST_WARN (12U * 1024U)

static const char *TAG = "lcd_driver";

typedef struct {
    lcd_driver_state_t state;
    bool spi_bus_owned;
    bool lvgl_port_started;
    bool legacy_released;
    uint16_t *legacy_dma_buffer;
    esp_lcd_panel_io_handle_t io;
    esp_lcd_panel_handle_t panel;
    lv_display_t *display;
    lcd_driver_lvgl_release_fn lvgl_release;
    void *lvgl_user_ctx;
    bool lvgl_pool_prepared;
    lcd_driver_metrics_t metrics;
} lcd_driver_context_t;

static lcd_driver_context_t s_ctx;
static StaticSemaphore_t s_lock_storage;
static SemaphoreHandle_t s_lock;

typedef struct {
    const char *owner;
    size_t size;
    uint32_t caps;
} lcd_alloc_request_t;

static void lcd_driver_log_alloc_plan(const char *owner, uint32_t caps, size_t size, const char *region)
{
    ESP_LOGI(TAG,
             "MEM_ALLOC_PLAN owner=%s caps=0x%08lx size=%u region=%s",
             owner,
             (unsigned long)caps,
             (unsigned int)size,
             region);
}

static void lcd_driver_capture_metrics(void)
{
    const uint32_t internal_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    const uint32_t dma_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT;
    const uint32_t psram_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;

    s_ctx.metrics.state = s_ctx.state;
    s_ctx.metrics.legacy_dma_bytes = s_ctx.legacy_dma_buffer != NULL ? LCD_LEGACY_DMA_BYTES : 0U;
    s_ctx.metrics.steady_dma_bytes = s_ctx.display != NULL ? LCD_LVGL_DRAW_BYTES : 0U;
    s_ctx.metrics.internal_free = heap_caps_get_free_size(internal_caps);
    s_ctx.metrics.internal_largest = heap_caps_get_largest_free_block(internal_caps);
    s_ctx.metrics.dma_free = heap_caps_get_free_size(dma_caps);
    s_ctx.metrics.dma_largest = heap_caps_get_largest_free_block(dma_caps);
    s_ctx.metrics.psram_free = heap_caps_get_free_size(psram_caps);
    s_ctx.metrics.psram_largest = heap_caps_get_largest_free_block(psram_caps);
    s_ctx.metrics.spi_bus_owned = s_ctx.spi_bus_owned;
    s_ctx.metrics.legacy_released = s_ctx.legacy_released;
}

static void lcd_driver_log_memory(const char *stage)
{
    lcd_driver_capture_metrics();
    ESP_LOGI(TAG,
             "LCD_MEM stage=%s internal_free=%u internal_largest=%u dma_free=%u dma_largest=%u psram_free=%u psram_largest=%u legacy=%u draw=%u",
             stage,
             (unsigned)s_ctx.metrics.internal_free,
             (unsigned)s_ctx.metrics.internal_largest,
             (unsigned)s_ctx.metrics.dma_free,
             (unsigned)s_ctx.metrics.dma_largest,
             (unsigned)s_ctx.metrics.psram_free,
             (unsigned)s_ctx.metrics.psram_largest,
             (unsigned)s_ctx.metrics.legacy_dma_bytes,
             (unsigned)s_ctx.metrics.steady_dma_bytes);
}

static void lcd_driver_log_lvgl_stage(const char *stage)
{
    lv_theme_t *theme = NULL;
    lv_draw_buf_t *draw_buf = NULL;
    void *buffer = NULL;
    if (s_ctx.display != NULL) {
        theme = lv_display_get_theme(s_ctx.display);
        draw_buf = lv_display_get_buf_active(s_ctx.display);
        if (draw_buf != NULL) {
            buffer = draw_buf->data;
        }
    }
    ESP_LOGI(TAG,
             "LVGL_INIT_STAGE %s display=%p theme=%p version=%d.%d.%d buffer=%p",
             stage,
             (void *)s_ctx.display,
             (void *)theme,
             lv_version_major(),
             lv_version_minor(),
             lv_version_patch(),
             buffer);
}

static esp_err_t lcd_driver_admit_plan(const char *stage,
                                       const lcd_alloc_request_t *plan,
                                       size_t plan_count)
{
    if (plan == NULL || plan_count == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    for (size_t i = 0U; i < plan_count; ++i) {
        size_t required_free = 0U;
        size_t required_largest = 0U;
        for (size_t j = 0U; j < plan_count; ++j) {
            if (plan[j].caps != plan[i].caps) {
                continue;
            }
            required_free += plan[j].size;
            if (plan[j].size > required_largest) {
                required_largest = plan[j].size;
            }
        }

        const size_t available_free = heap_caps_get_free_size(plan[i].caps);
        const size_t available_largest = heap_caps_get_largest_free_block(plan[i].caps);
        if (available_free < required_free || available_largest < required_largest) {
            ESP_LOGE(TAG,
                     "LCD_MEM_ADMISSION_FAIL stage=%s owner=%s caps=0x%08lx available_free=%u available_largest=%u required_free=%u required_largest=%u",
                     stage,
                     plan[i].owner,
                     (unsigned long)plan[i].caps,
                     (unsigned)available_free,
                     (unsigned)available_largest,
                     (unsigned)required_free,
                     (unsigned)required_largest);
            return ESP_ERR_NO_MEM;
        }
    }
    return ESP_OK;
}

static esp_err_t lcd_driver_lock(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutexStatic(&s_lock_storage);
    }
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    return xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

static void lcd_driver_unlock(void)
{
    (void)xSemaphoreGive(s_lock);
}

static void lcd_driver_set_backlight(bool enabled)
{
    if (LCD_BOARD_BL == GPIO_NUM_NC) {
        return;
    }
    (void)gpio_set_level(LCD_BOARD_BL, enabled ? LCD_BOARD_BL_ON_LEVEL : !LCD_BOARD_BL_ON_LEVEL);
}

static uint64_t lcd_driver_gpio_mask(gpio_num_t gpio)
{
    if (gpio < 0 || gpio >= 64) {
        return 0U;
    }
    return 1ULL << (uint32_t)gpio;
}

static esp_err_t lcd_driver_init_backlight(void)
{
    if (LCD_BOARD_BL == GPIO_NUM_NC) {
        return ESP_OK;
    }
    const gpio_config_t config = {
        .pin_bit_mask = lcd_driver_gpio_mask(LCD_BOARD_BL),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t ret = gpio_config(&config);
    if (ret == ESP_OK) {
        lcd_driver_set_backlight(false);
    }
    return ret;
}

static void lcd_driver_release_panel(void)
{
    if (s_ctx.panel != NULL) {
        (void)esp_lcd_panel_del(s_ctx.panel);
        s_ctx.panel = NULL;
    }
    if (s_ctx.io != NULL) {
        (void)esp_lcd_panel_io_del(s_ctx.io);
        s_ctx.io = NULL;
    }
    if (s_ctx.spi_bus_owned) {
        (void)spi_bus_free(LCD_BOARD_SPI_HOST);
        s_ctx.spi_bus_owned = false;
    }
}

static void lcd_driver_release_legacy_buffer(void)
{
    if (s_ctx.legacy_dma_buffer != NULL) {
        heap_caps_free(s_ctx.legacy_dma_buffer);
        s_ctx.legacy_dma_buffer = NULL;
    }
    s_ctx.legacy_released = true;
}

static void lcd_driver_reset_context(lcd_driver_state_t state)
{
    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.state = state;
    lcd_driver_capture_metrics();
}

esp_err_t lcd_driver_start(void)
{
    const esp_err_t lock_ret = lcd_driver_lock();
    if (lock_ret != ESP_OK) {
        return lock_ret;
    }
    if (s_ctx.state == LCD_DRIVER_READY || s_ctx.state == LCD_DRIVER_PUBLISHED) {
        lcd_driver_unlock();
        return ESP_OK;
    }
    if (s_ctx.state == LCD_DRIVER_ALLOCATING || s_ctx.state == LCD_DRIVER_STOPPING) {
        lcd_driver_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    lcd_driver_reset_context(LCD_DRIVER_ALLOCATING);
    const lcd_alloc_request_t bootstrap_plan[] = {
        {
            .owner = "lcd_legacy_dma",
            .size = LCD_LEGACY_DMA_BYTES,
            .caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT,
        },
    };
    esp_err_t ret;

    ret = lcd_driver_init_backlight();
    if (ret != ESP_OK) {
        goto fail;
    }

    const spi_bus_config_t bus_config = {
        .sclk_io_num = LCD_BOARD_SCLK,
        .mosi_io_num = LCD_BOARD_MOSI,
        .miso_io_num = LCD_BOARD_MISO,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = LCD_LEGACY_DMA_BYTES,
    };
    ret = spi_bus_initialize(LCD_BOARD_SPI_HOST, &bus_config, SPI_DMA_CH_AUTO);
    if (ret == ESP_OK) {
        s_ctx.spi_bus_owned = true;
    } else if (ret == ESP_ERR_INVALID_STATE) {
        size_t max_transfer = 0U;
        ret = spi_bus_get_max_transaction_len(LCD_BOARD_SPI_HOST, &max_transfer);
        if (ret != ESP_OK || max_transfer < LCD_LVGL_DRAW_BYTES) {
            ESP_LOGE(TAG, "SPI2 shared bus cannot satisfy LCD transfer len=%u got=%u ret=%s",
                     (unsigned)LCD_LVGL_DRAW_BYTES,
                     (unsigned)max_transfer,
                     esp_err_to_name(ret));
            ret = ESP_ERR_INVALID_STATE;
            goto fail;
        }
        ESP_LOGI(TAG, "LCD_SHARED_SPI2 verified max_transfer=%u", (unsigned)max_transfer);
        ret = ESP_OK;
    }
    if (ret != ESP_OK) {
        goto fail;
    }

    const esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = LCD_BOARD_CS,
        .dc_gpio_num = LCD_BOARD_DC,
        .spi_mode = 0,
        .pclk_hz = LCD_BOARD_PIXEL_CLOCK_HZ,
        .trans_queue_depth = LCD_BOARD_IO_QUEUE_DEPTH,
        .on_color_trans_done = NULL,
        .user_ctx = NULL,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    if (lcd_fault_injection_should_fail(LCD_FAULT_PANEL_IO)) { ret = ESP_ERR_NO_MEM; goto fail; }
    ret = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_BOARD_SPI_HOST,
                                   &io_config,
                                   &s_ctx.io);
    if (ret != ESP_OK) {
        goto fail;
    }

    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = LCD_BOARD_RST,
        .rgb_ele_order = LCD_BOARD_RGB_ORDER,
        .data_endian = LCD_RGB_DATA_ENDIAN_BIG,
        .bits_per_pixel = 16,
    };
    if (lcd_fault_injection_should_fail(LCD_FAULT_PANEL_DRIVER)) { ret = ESP_ERR_NO_MEM; goto fail; }
    ret = esp_lcd_new_panel_st7789(s_ctx.io, &panel_config, &s_ctx.panel);
    if (ret != ESP_OK) {
        goto fail;
    }
    if ((ret = esp_lcd_panel_reset(s_ctx.panel)) != ESP_OK ||
        (ret = esp_lcd_panel_init(s_ctx.panel)) != ESP_OK ||
        (ret = esp_lcd_panel_invert_color(s_ctx.panel, LCD_BOARD_INVERT)) != ESP_OK ||
        (ret = esp_lcd_panel_swap_xy(s_ctx.panel, LCD_BOARD_SWAP_XY)) != ESP_OK ||
        (ret = esp_lcd_panel_mirror(s_ctx.panel, LCD_BOARD_MIRROR_X, LCD_BOARD_MIRROR_Y)) != ESP_OK ||
        (ret = esp_lcd_panel_set_gap(s_ctx.panel, LCD_BOARD_X_GAP, LCD_BOARD_Y_GAP)) != ESP_OK ||
        (ret = esp_lcd_panel_disp_on_off(s_ctx.panel, true)) != ESP_OK) {
        goto fail;
    }

    if (lcd_fault_injection_should_fail(LCD_FAULT_LEGACY_DMA)) { ret = ESP_ERR_NO_MEM; goto fail; }
    const uint32_t legacy_dma_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT;
    ret = lcd_driver_admit_plan("bootstrap",
                                bootstrap_plan,
                                sizeof(bootstrap_plan) / sizeof(bootstrap_plan[0]));
    if (ret != ESP_OK) {
        goto fail;
    }
    lcd_driver_log_alloc_plan(bootstrap_plan[0].owner,
                              legacy_dma_caps,
                              bootstrap_plan[0].size,
                              "internal_dma");
    s_ctx.legacy_dma_buffer = heap_caps_calloc(1, LCD_LEGACY_DMA_BYTES, legacy_dma_caps);
    if (s_ctx.legacy_dma_buffer == NULL || !esp_ptr_dma_capable(s_ctx.legacy_dma_buffer) ||
        !esp_ptr_internal(s_ctx.legacy_dma_buffer)) {
        ESP_LOGE(TAG, "LCD legacy DMA allocation/capability validation failed");
        ret = ESP_ERR_NO_MEM;
        goto fail;
    }

    lcd_driver_set_backlight(true);
    s_ctx.state = LCD_DRIVER_READY;
    lcd_driver_log_memory("panel_ready_legacy_4800");
    lcd_driver_unlock();
    return ESP_OK;

fail:
    lcd_driver_set_backlight(false);
    lcd_driver_release_legacy_buffer();
    lcd_driver_release_panel();
    s_ctx.state = LCD_DRIVER_FAILED;
    lcd_driver_log_memory("start_failed");
    lcd_driver_unlock();
    return ret;
}

esp_err_t lcd_driver_register_lvgl(lcd_driver_lvgl_prepare_fn prepare,
                                   lcd_driver_lvgl_release_fn release,
                                   void *user_ctx)
{
    const esp_err_t lock_ret = lcd_driver_lock();
    if (lock_ret != ESP_OK) {
        return lock_ret;
    }
    if (s_ctx.state == LCD_DRIVER_PUBLISHED) {
        lcd_driver_unlock();
        return ESP_OK;
    }
    if (s_ctx.state != LCD_DRIVER_READY || s_ctx.panel == NULL || s_ctx.io == NULL ||
        s_ctx.legacy_dma_buffer == NULL || prepare == NULL || release == NULL) {
        lcd_driver_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    const lvgl_port_cfg_t lvgl_cfg = {
        .task_priority = 1,
        .task_stack = 4096,
        .task_affinity = -1,
        .task_max_sleep_ms = 1000,
        .task_stack_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
        .timer_period_ms = 20,
    };
    lcd_driver_log_alloc_plan("lcd_lvgl_port_stack", lvgl_cfg.task_stack_caps, lvgl_cfg.task_stack, "psram");
    bool lvgl_locked = false;
    esp_err_t ret = lcd_fault_injection_should_fail(LCD_FAULT_LVGL_PORT) ? ESP_ERR_NO_MEM : lvgl_port_init(&lvgl_cfg);
    if (ret != ESP_OK) {
        goto fail;
    }
    s_ctx.lvgl_port_started = true;

    if (!lvgl_port_lock(1000U)) {
        ret = ESP_ERR_TIMEOUT;
        goto fail;
    }
    lvgl_locked = true;
    ret = prepare(user_ctx);
    if (ret != ESP_OK) {
        goto fail;
    }
    s_ctx.lvgl_release = release;
    s_ctx.lvgl_user_ctx = user_ctx;
    s_ctx.lvgl_pool_prepared = true;

    const lvgl_port_display_cfg_t display_cfg = {
        .io_handle = s_ctx.io,
        .panel_handle = s_ctx.panel,
        .control_handle = NULL,
        .buffer_size = LCD_BOARD_HRES * LCD_LVGL_DRAW_LINES,
        .double_buffer = false,
        .trans_size = 0,
        .hres = LCD_BOARD_HRES,
        .vres = LCD_BOARD_VRES,
        .monochrome = false,
        .rotation = {
            .swap_xy = LCD_BOARD_SWAP_XY,
            .mirror_x = LCD_BOARD_MIRROR_X,
            .mirror_y = LCD_BOARD_MIRROR_Y,
        },
        .color_format = LV_COLOR_FORMAT_RGB565,
        .flags = {
            .buff_dma = true,
            .buff_spiram = false,
            .sw_rotate = false,
            .swap_bytes = true,
            .full_refresh = false,
            .direct_mode = false,
        },
    };
    const lcd_alloc_request_t steady_plan[] = {
        {
            .owner = "lcd_lvgl_draw_buffer",
            .size = display_cfg.buffer_size * sizeof(uint16_t) * (display_cfg.double_buffer ? 2U : 1U),
            .caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT,
        },
    };
    ret = lcd_driver_admit_plan("lvgl_draw_buffer",
                                steady_plan,
                                sizeof(steady_plan) / sizeof(steady_plan[0]));
    if (ret != ESP_OK) {
        goto fail;
    }
    lcd_driver_log_alloc_plan(steady_plan[0].owner,
                              steady_plan[0].caps,
                              steady_plan[0].size,
                              "internal_dma");
    /* LVGL9 creates its default theme synchronously inside lv_display_create(). */
    lcd_driver_log_lvgl_stage("before_display_create");
    lcd_driver_log_lvgl_stage("before_theme_init");
    s_ctx.display = lcd_fault_injection_should_fail(LCD_FAULT_LVGL_DISPLAY) ? NULL : lvgl_port_add_disp(&display_cfg);
    lcd_driver_log_lvgl_stage("after_theme_init");
    lcd_driver_log_lvgl_stage("after_display_create");
    if (s_ctx.display == NULL) {
        ret = ESP_ERR_NO_MEM;
        goto fail;
    }
    lv_draw_buf_t *const draw_buf = lv_display_get_buf_active(s_ctx.display);
    if (draw_buf == NULL || draw_buf->data == NULL || draw_buf->data_size != LCD_LVGL_DRAW_BYTES ||
        !esp_ptr_dma_capable(draw_buf->data) || !esp_ptr_internal(draw_buf->data)) {
        ESP_LOGE(TAG, "LCD LVGL draw buffer did not meet internal DMA contract");
        ret = ESP_ERR_NO_MEM;
        goto fail;
    }

    /* The 4800 B bootstrap buffer is no longer reachable after this point. */
    lcd_driver_release_legacy_buffer();
    s_ctx.state = LCD_DRIVER_PUBLISHED;
    lcd_driver_log_memory("lvgl_registered_draw_4800");
    if (s_ctx.metrics.dma_largest < LCD_RUNTIME_DMA_LARGEST_WARN) {
        ESP_LOGW(TAG, "LCD_DMA_LARGEST_LOW largest=%u", (unsigned)s_ctx.metrics.dma_largest);
    }
    lvgl_port_unlock();
    lvgl_locked = false;
    lcd_driver_unlock();
    return ESP_OK;

fail:
    lcd_driver_log_lvgl_stage("failed");
    if (s_ctx.display != NULL) {
        (void)lvgl_port_remove_disp(s_ctx.display);
        s_ctx.display = NULL;
    }
    if (lvgl_locked) {
        lvgl_port_unlock();
    }
    if (s_ctx.lvgl_port_started) {
        (void)lvgl_port_deinit();
        s_ctx.lvgl_port_started = false;
    }
    if (s_ctx.lvgl_pool_prepared && s_ctx.lvgl_release != NULL) {
        s_ctx.lvgl_release(s_ctx.lvgl_user_ctx);
        s_ctx.lvgl_pool_prepared = false;
    }
    lcd_driver_release_legacy_buffer();
    lcd_driver_set_backlight(false);
    lcd_driver_release_panel();
    s_ctx.state = LCD_DRIVER_FAILED;
    lcd_driver_log_memory("lvgl_register_failed");
    lcd_driver_unlock();
    return ret;
}

esp_err_t lcd_driver_stop(void)
{
    const esp_err_t lock_ret = lcd_driver_lock();
    if (lock_ret != ESP_OK) {
        return lock_ret;
    }
    if (s_ctx.state == LCD_DRIVER_UNINITIALIZED) {
        lcd_driver_unlock();
        return ESP_OK;
    }
    s_ctx.state = LCD_DRIVER_STOPPING;
    lcd_driver_set_backlight(false);
    if (s_ctx.lvgl_port_started && !lvgl_port_lock(1000U)) {
        lcd_driver_unlock();
        return ESP_ERR_TIMEOUT;
    }
    if (s_ctx.display != NULL) {
        (void)lvgl_port_remove_disp(s_ctx.display);
        s_ctx.display = NULL;
    }
    if (s_ctx.lvgl_port_started) {
        lvgl_port_unlock();
    }
    if (s_ctx.lvgl_port_started) {
        (void)lvgl_port_deinit();
        s_ctx.lvgl_port_started = false;
    }
    if (s_ctx.lvgl_pool_prepared && s_ctx.lvgl_release != NULL) {
        s_ctx.lvgl_release(s_ctx.lvgl_user_ctx);
        s_ctx.lvgl_pool_prepared = false;
    }
    lcd_driver_release_legacy_buffer();
    lcd_driver_release_panel();
    lcd_driver_reset_context(LCD_DRIVER_UNINITIALIZED);
    lcd_driver_log_memory("stopped");
    lcd_driver_unlock();
    return ESP_OK;
}

bool lcd_driver_is_ready(void)
{
    return s_ctx.state == LCD_DRIVER_READY || s_ctx.state == LCD_DRIVER_PUBLISHED;
}

lcd_driver_state_t lcd_driver_get_state(void)
{
    return s_ctx.state;
}

lv_display_t *lcd_driver_get_display(void)
{
    return s_ctx.display;
}

esp_lcd_panel_io_handle_t lcd_driver_get_io_handle(void)
{
    return s_ctx.io;
}

esp_lcd_panel_handle_t lcd_driver_get_panel_handle(void)
{
    return s_ctx.panel;
}

void lcd_driver_get_metrics(lcd_driver_metrics_t *out_metrics)
{
    if (out_metrics == NULL) {
        return;
    }
    lcd_driver_capture_metrics();
    *out_metrics = s_ctx.metrics;
}

esp_err_t lcd_driver_fill_legacy(uint16_t rgb565)
{
    if (s_ctx.state != LCD_DRIVER_READY || s_ctx.legacy_dma_buffer == NULL || s_ctx.panel == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    const size_t pixels = LCD_LEGACY_DMA_BYTES / sizeof(uint16_t);
    for (size_t i = 0; i < pixels; ++i) {
        s_ctx.legacy_dma_buffer[i] = (uint16_t)((rgb565 << 8U) | (rgb565 >> 8U));
    }
    for (uint16_t y = 0; y < LCD_BOARD_VRES; y += LCD_LVGL_DRAW_LINES) {
        const uint16_t height = (uint16_t)((LCD_BOARD_VRES - y) < LCD_LVGL_DRAW_LINES ?
                                             (LCD_BOARD_VRES - y) : LCD_LVGL_DRAW_LINES);
        esp_err_t ret = esp_lcd_panel_draw_bitmap(s_ctx.panel,
                                                   0,
                                                   y,
                                                   LCD_BOARD_HRES,
                                                   y + height,
                                                   s_ctx.legacy_dma_buffer);
        if (ret != ESP_OK) {
            return ret;
        }
    }
    return ESP_OK;
}
