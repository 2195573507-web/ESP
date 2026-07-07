/**
 * @file bme_sensor_service.c
 * @brief C5 终端 BME690 后台读取与上报服务。
 *
 * 本文件属于 ESP32-C5 终端（ESPC51/ESPC52 共用），负责注册 BME690 scheduler
 * tick，调用 bme690 driver 读取数据、调用 bme_air_quality 计算空气质量，并通过
 * bme_server_client 上传到 S3。本文件不实现 I2C 底层、不改变统一设备流协议，
 * 也不参与 voice PCM 代理；语音活跃时只按 runtime gate 暂停/恢复本服务。
 */

#include "bme_sensor_service.h"

#include <stdbool.h>
#include <stdint.h>

#include "app_debug_config.h"
#include "app_runtime.h"
#include "app_stack_monitor.h"
#include "bme_air_quality.h"
#include "bme690.h"
#include "bme_server_client.h"
#include "c5_backpressure_controller.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "bme_sensor_service";

typedef struct {
    bool running;
    bool paused;
    bool busy;
    bool initialized;
    bool stop_requested;
} bme_sensor_service_context_t;

static bme_sensor_service_context_t s_bme_service;
static portMUX_TYPE s_bme_service_lock = portMUX_INITIALIZER_UNLOCKED;

static void bme_sensor_service_log_heap(const char *label)
{
#if ENABLE_VERBOSE_AUDIO_LOG
    ESP_LOGI(TAG,
             "%s free_heap=%u min_free_heap=%u largest_free_block=%u",
             label != NULL ? label : "BME service heap",
             (unsigned int)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned int)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT),
             (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
#else
    (void)label;
#endif
}

static bool bme_sensor_service_snapshot(bool *paused,
                                        bool *busy,
                                        bool *initialized,
                                        bool *stop_requested)
{
    bool running;

    portENTER_CRITICAL(&s_bme_service_lock);
    running = s_bme_service.running;
    if (paused != NULL) {
        *paused = s_bme_service.paused;
    }
    if (busy != NULL) {
        *busy = s_bme_service.busy;
    }
    if (initialized != NULL) {
        *initialized = s_bme_service.initialized;
    }
    if (stop_requested != NULL) {
        *stop_requested = s_bme_service.stop_requested;
    }
    portEXIT_CRITICAL(&s_bme_service_lock);

    return running;
}

static void bme_sensor_service_mark_busy(bool busy)
{
    portENTER_CRITICAL(&s_bme_service_lock);
    s_bme_service.busy = busy;
    portEXIT_CRITICAL(&s_bme_service_lock);
}

static void bme_sensor_service_mark_initialized(bool initialized)
{
    portENTER_CRITICAL(&s_bme_service_lock);
    s_bme_service.initialized = initialized;
    portEXIT_CRITICAL(&s_bme_service_lock);
}

static void bme_sensor_service_mark_stopped(void)
{
    portENTER_CRITICAL(&s_bme_service_lock);
    s_bme_service.running = false;
    s_bme_service.paused = false;
    s_bme_service.busy = false;
    s_bme_service.stop_requested = false;
    portEXIT_CRITICAL(&s_bme_service_lock);
}

static esp_err_t bme_sensor_service_init_once(void)
{
    bool initialized = false;
    bool stop_requested = false;
    (void)bme_sensor_service_snapshot(NULL, NULL, &initialized, &stop_requested);
    if (initialized) {
        return ESP_OK;
    }
    if (stop_requested) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGD(TAG, "BME init start");
    esp_err_t ret = bme_server_client_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "BME upload bridge init fail: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = bme690_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "BME init fail: %s", esp_err_to_name(ret));
        return ret;
    }

    bme_sensor_service_mark_initialized(true);
    ESP_LOGI(TAG, "BME init success");
    return ESP_OK;
}

static void bme_sensor_service_delay_ms(uint32_t delay_ms)
{
    TickType_t ticks = pdMS_TO_TICKS(delay_ms);
    if (ticks == 0) {
        ticks = 1;
    }
    vTaskDelay(ticks);
}

static void bme_sensor_service_mark_stopped_if_requested(void)
{
    bool stop_requested = false;
    bool busy = false;
    bool running = bme_sensor_service_snapshot(NULL, &busy, NULL, &stop_requested);
    if (running && stop_requested && !busy) {
        bme_sensor_service_mark_stopped();
    }
}

esp_err_t bme_sensor_service_tick(void)
{
    if (!c5_should_run(C5_TASK_TYPE_BME_SENSOR)) {
        return ESP_ERR_INVALID_STATE;
    }

    bool paused = false;
    bool stop_requested = false;
    bool running = bme_sensor_service_snapshot(&paused, NULL, NULL, &stop_requested);
    if (!running || paused || stop_requested) {
        bme_sensor_service_mark_stopped_if_requested();
        return ESP_ERR_INVALID_STATE;
    }

    bme_sensor_service_mark_busy(true);
    esp_err_t ret = bme_sensor_service_init_once();
    if (ret != ESP_OK) {
        goto done;
    }

    paused = false;
    stop_requested = false;
    (void)bme_sensor_service_snapshot(&paused, NULL, NULL, &stop_requested);
    if (paused || stop_requested) {
        ret = ESP_ERR_INVALID_STATE;
        goto done;
    }

    bme690_data_t sensor_data = {0};
    ret = bme690_read(&sensor_data);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "BME read fail: %s", esp_err_to_name(ret));
        goto done;
    }

    ESP_LOGD(TAG,
             "BME read success temp=%.2fC hum=%.2f%% pressure=%.2fhPa gas=%luOhm",
             sensor_data.temperature_c,
             sensor_data.humidity_percent,
             sensor_data.pressure_hpa,
             (unsigned long)sensor_data.gas_resistance_ohm);

    paused = false;
    stop_requested = false;
    (void)bme_sensor_service_snapshot(&paused, NULL, NULL, &stop_requested);
    if (paused || stop_requested) {
        ret = ESP_ERR_INVALID_STATE;
        goto done;
    }

    bme_air_quality_result_t air_quality = {0};
    ret = bme_air_quality_update(&sensor_data, &air_quality);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "BME air quality calc fail: %s", esp_err_to_name(ret));
        goto done;
    }

    ret = bme_server_client_upload_reading(BME_SENSOR_DEVICE_ID,
                                           &sensor_data,
                                           &air_quality);
    if (ret == ESP_OK) {
        ESP_LOGD(TAG, "BME upload success");
    } else if (ret == ESP_ERR_INVALID_STATE && app_runtime_non_voice_is_paused()) {
        app_runtime_log_voice_busy_skip("BME upload");
    } else {
        ESP_LOGW(TAG, "BME upload fail: %s", esp_err_to_name(ret));
        app_stack_monitor_log(TAG, "bme_sensor_tick", "upload_error");
        bme_sensor_service_log_heap("BME upload fail heap");
    }

done:
    bme_sensor_service_mark_busy(false);
    bme_sensor_service_mark_stopped_if_requested();
    return ret;
}

esp_err_t bme_sensor_service_start(void)
{
    portENTER_CRITICAL(&s_bme_service_lock);
    if (s_bme_service.running) {
        portEXIT_CRITICAL(&s_bme_service_lock);
        ESP_LOGD(TAG, "BME service start requested but already running");
        bme_sensor_service_log_heap("BME start already running heap");
        return ESP_OK;
    }
    s_bme_service.running = true;
    s_bme_service.paused = false;
    s_bme_service.busy = false;
    s_bme_service.stop_requested = false;
    portEXIT_CRITICAL(&s_bme_service_lock);

    ESP_LOGD(TAG, "BME service registered with C5 scheduler");
    bme_sensor_service_log_heap("BME start heap");
    return ESP_OK;
}

esp_err_t bme_sensor_service_wait_paused(uint32_t timeout_ms)
{
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);

    while (1) {
        bool paused = false;
        bool busy = false;
        bool running = bme_sensor_service_snapshot(&paused, &busy, NULL, NULL);
        if (!running || (paused && !busy)) {
            return ESP_OK;
        }
        if ((int32_t)(deadline - xTaskGetTickCount()) <= 0) {
            ESP_LOGW(TAG,
                     "BME service wait paused timeout: paused=%d busy=%d timeout_ms=%u",
                     paused ? 1 : 0,
                     busy ? 1 : 0,
                     (unsigned int)timeout_ms);
            bme_sensor_service_log_heap("BME wait paused timeout heap");
            return ESP_ERR_TIMEOUT;
        }
        bme_sensor_service_delay_ms(BME_SENSOR_WAIT_PAUSED_POLL_MS);
    }
}

void bme_sensor_service_pause(void)
{
    bool should_log = false;

    portENTER_CRITICAL(&s_bme_service_lock);
    if (s_bme_service.running && !s_bme_service.paused) {
        s_bme_service.paused = true;
        should_log = true;
    }
    portEXIT_CRITICAL(&s_bme_service_lock);

    if (should_log) {
        ESP_LOGD(TAG, "BME service pause because voice active");
        bme_sensor_service_log_heap("BME pause heap");
    }
}

void bme_sensor_service_resume(void)
{
    bool should_log = false;

    portENTER_CRITICAL(&s_bme_service_lock);
    if (s_bme_service.running && s_bme_service.paused) {
        s_bme_service.paused = false;
        should_log = true;
    }
    portEXIT_CRITICAL(&s_bme_service_lock);

    if (should_log) {
        ESP_LOGD(TAG, "BME service resume after voice done");
        bme_sensor_service_log_heap("BME resume heap");
    }
}

void bme_sensor_service_stop(void)
{
    portENTER_CRITICAL(&s_bme_service_lock);
    if (s_bme_service.running) {
        s_bme_service.stop_requested = true;
    }
    portEXIT_CRITICAL(&s_bme_service_lock);

    if (!bme_sensor_service_is_running()) {
        return;
    }

    ESP_LOGD(TAG, "BME service stop requested");
    bme_sensor_service_mark_stopped_if_requested();

    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(BME_SENSOR_STOP_JOIN_TIMEOUT_MS);
    while (bme_sensor_service_is_running()) {
        if ((int32_t)(deadline - xTaskGetTickCount()) <= 0) {
            ESP_LOGW(TAG, "BME service stop wait timeout");
            break;
        }
        bme_sensor_service_delay_ms(50);
    }
}

bool bme_sensor_service_is_running(void)
{
    return bme_sensor_service_snapshot(NULL, NULL, NULL, NULL);
}

bool bme_sensor_service_is_paused(void)
{
    bool paused = false;
    (void)bme_sensor_service_snapshot(&paused, NULL, NULL, NULL);
    return paused;
}
