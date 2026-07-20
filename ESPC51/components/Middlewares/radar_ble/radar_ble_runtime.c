#include "radar_ble_runtime.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "radar_ble_binding_config.h"
#include "radar_worker.h"
#include "esp_heap_caps.h"

static const char *TAG = "radar_ble_runtime";
static TaskHandle_t s_task;
static bool s_started;
static radar_ble_state_t s_last_state;

static uint64_t now_ms(void)
{
    const int64_t value = esp_timer_get_time();
    return value > 0 ? (uint64_t)(value / 1000) : 0U;
}

static void notify_cb(const uint8_t *data, size_t length, void *ctx)
{
    (void)ctx;
    radar_domain_notify(data, length, now_ms());
}

static void process_task(void *arg)
{
    (void)arg;
    while (true) {
        radar_ble_transport_status_t status = {0};
        radar_ble_transport_get_status(&status);
        const bool link_online = status.connected && status.notify_subscribed;
        radar_domain_set_link_state((uint8_t)status.state, link_online);
        if (status.state != s_last_state) {
            s_last_state = status.state;
            ESP_LOGI(TAG,
                     "RADAR_SOURCE_STATE event=ble_state source_id=%u source=C51 device_id=%s room=%s sequence=0 local_id=%u state=%s link_online=%u",
                     (unsigned int)RADAR_BLE_BINDING_LOCAL_ID,
                     RADAR_BLE_BINDING_DEVICE_ID,
                     RADAR_BLE_BINDING_ROOM_ID,
                     (unsigned int)RADAR_BLE_BINDING_LOCAL_ID,
                     radar_ble_state_name(status.state),
                     link_online ? 1U : 0U);
        }

        radar_domain_mark_timeout(now_ms());
        if (link_online) radar_ble_transport_set_data_ready(true);
        if (!link_online) {
            radar_ble_transport_set_data_ready(false);
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

esp_err_t radar_ble_runtime_start(void)
{
    if (s_started) {
        return ESP_OK;
    }
    const esp_err_t ret = radar_domain_start();
    if (ret != ESP_OK) {
        return ret;
    }
    if (xTaskCreateWithCaps(process_task, "radar_ble_rx", 2048, NULL, 2, &s_task,
                            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) != pdPASS) {
        s_task = NULL;
        radar_domain_stop();
        return ESP_ERR_NO_MEM;
    }
    s_started = true;
    s_last_state = RADAR_BLE_STATE_DISABLED;
    const int transport_ret = radar_ble_transport_start(notify_cb, NULL);
    if (transport_ret != 0) {
        ESP_LOGW(TAG,
                 "RADAR_SOURCE_STATE event=ble_unavailable source_id=%u source=C51 device_id=%s room=%s sequence=0 local_id=%u reason=BLOCKED_BY_RADAR_GATT_UUID ret=%d",
                 (unsigned int)RADAR_BLE_BINDING_LOCAL_ID,
                 RADAR_BLE_BINDING_DEVICE_ID,
                 RADAR_BLE_BINDING_ROOM_ID,
                 (unsigned int)RADAR_BLE_BINDING_LOCAL_ID,
                 transport_ret);
    }
    return ESP_OK;
}

void radar_ble_runtime_stop(void)
{
    radar_ble_transport_stop();
    if (s_task != NULL) {
        vTaskDelete(s_task);
        s_task = NULL;
    }
    s_started = false;
    s_last_state = RADAR_BLE_STATE_DISABLED;
    radar_domain_stop();
}

void radar_ble_runtime_get_status(radar_ble_transport_status_t *out)
{
    radar_ble_transport_get_status(out);
}
