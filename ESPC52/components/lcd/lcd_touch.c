#include "lcd_touch.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

#include "iic.h"
#include "lcd_fault_injection.h"

#define LCD_TOUCH_I2C_ADDRESS 0x15U
#define LCD_TOUCH_REG_CHIP_ID 0xA7U
#define LCD_TOUCH_REG_POINT 0x01U
#define LCD_TOUCH_POINT_BYTES 6U
#define LCD_TOUCH_PERIOD_MS 40U
#define LCD_TOUCH_TASK_STACK 2048U

static const char *TAG = "lcd_touch";
static portMUX_TYPE s_touch_lock = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t s_task;
static TaskHandle_t s_stop_waiter;
static volatile bool s_stop_requested;
static bool s_available;
static lcd_touch_point_t s_latest;

static esp_err_t lcd_touch_read_register(uint8_t reg, uint8_t *out, size_t length)
{
    if (out == NULL || length == 0U || IIC_MASTER_PORT >= I2C_NUM_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    i2c_obj_t *const bus = &iic_master[IIC_MASTER_PORT];
    if (bus->init_flag != ESP_OK || bus->bus_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return iic_read(bus, LCD_TOUCH_I2C_ADDRESS, &reg, sizeof(reg), out, length);
}

static void lcd_touch_publish(uint16_t x, uint16_t y, bool pressed)
{
    portENTER_CRITICAL(&s_touch_lock);
    s_latest.x = x;
    s_latest.y = y;
    s_latest.pressed = pressed;
    ++s_latest.generation;
    portEXIT_CRITICAL(&s_touch_lock);
}

static void lcd_touch_task(void *arg)
{
    (void)arg;
    while (!s_stop_requested) {
        uint8_t data[LCD_TOUCH_POINT_BYTES] = {0};
        const esp_err_t ret = lcd_touch_read_register(LCD_TOUCH_REG_POINT, data, sizeof(data));
        if (ret == ESP_OK && data[1] != 0U) {
            const uint16_t x = (uint16_t)((((uint16_t)data[2] & 0x0FU) << 8U) | data[3]);
            const uint16_t y = (uint16_t)((((uint16_t)data[4] & 0x0FU) << 8U) | data[5]);
            lcd_touch_publish(x, y, true);
        } else {
            lcd_touch_publish(0U, 0U, false);
        }
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(LCD_TOUCH_PERIOD_MS));
    }

    portENTER_CRITICAL(&s_touch_lock);
    s_available = false;
    s_task = NULL;
    portEXIT_CRITICAL(&s_touch_lock);
    if (s_stop_waiter != NULL) {
        xTaskNotifyGive(s_stop_waiter);
    }
    vTaskDeleteWithCaps(NULL);
}

esp_err_t lcd_touch_start(void)
{
    if (s_task != NULL) {
        return ESP_OK;
    }
    if (IIC_MASTER_PORT >= I2C_NUM_MAX || iic_master[IIC_MASTER_PORT].init_flag != ESP_OK ||
        iic_master[IIC_MASTER_PORT].bus_handle == NULL) {
        ESP_LOGW(TAG, "LCD_TOUCH_DEGRADED existing I2C0 bus is not ready");
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t chip_id = 0U;
    const esp_err_t probe_ret = lcd_touch_read_register(LCD_TOUCH_REG_CHIP_ID, &chip_id, sizeof(chip_id));
    if (probe_ret != ESP_OK) {
        ESP_LOGW(TAG, "LCD_TOUCH_DEGRADED CST816T probe failed: %s", esp_err_to_name(probe_ret));
        return probe_ret;
    }

    s_stop_requested = false;
    if (lcd_fault_injection_should_fail(LCD_FAULT_TOUCH_TASK)) return ESP_ERR_NO_MEM;
    const uint32_t task_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    ESP_LOGI(TAG,
             "MEM_ALLOC_PLAN owner=lcd_touch_stack caps=0x%08lx size=%u region=psram",
             (unsigned long)task_caps,
             (unsigned int)LCD_TOUCH_TASK_STACK);
    const BaseType_t created = xTaskCreateWithCaps(lcd_touch_task,
                                                   "lcd_touch",
                                                   LCD_TOUCH_TASK_STACK,
                                                   NULL,
                                                   1,
                                                   &s_task,
                                                   task_caps);
    if (created != pdPASS || s_task == NULL) {
        s_task = NULL;
        ESP_LOGE(TAG, "LCD_TOUCH task create failed");
        return ESP_ERR_NO_MEM;
    }
    s_available = true;
    ESP_LOGI(TAG, "LCD_TOUCH_READY chip=0x%02X stack=psram", chip_id);
    return ESP_OK;
}

esp_err_t lcd_touch_stop(void)
{
    TaskHandle_t task = s_task;
    if (task == NULL) {
        return ESP_OK;
    }
    s_stop_waiter = xTaskGetCurrentTaskHandle();
    s_stop_requested = true;
    xTaskNotifyGive(task);
    if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(250U)) == 0U) {
        ESP_LOGE(TAG, "LCD_TOUCH stop timeout; task remains owned for retry");
        return ESP_ERR_TIMEOUT;
    }
    s_stop_waiter = NULL;
    return ESP_OK;
}

bool lcd_touch_is_available(void)
{
    return s_available;
}

void lcd_touch_get_latest(lcd_touch_point_t *out_point)
{
    if (out_point == NULL) {
        return;
    }
    portENTER_CRITICAL(&s_touch_lock);
    *out_point = s_latest;
    portEXIT_CRITICAL(&s_touch_lock);
}
