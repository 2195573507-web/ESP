/**
 * @file app_orchestrator.c
 * @brief C5 终端启动编排实现。
 *
 * 本文件属于 ESP32-C5 终端（ESPC51/ESPC52 共用），负责把启动链路串起来：
 * app_main -> app_orchestrator_start -> WiFi -> system/register/command -> BME -> voice。
 * 本文件不实现 WiFi 状态机、BME 驱动、Mic/VAD、语音代理、命令协议或 LCD/CSI 逻辑；
 * 这些职责分别由 wifi_manager、system_service、bme_sensor_service、voice_chain、
 * display_placeholder 和 csi_placeholder 等模块承担。
 */

#include "app_orchestrator.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "app_debug_config.h"
#include "app_main_config.h"
#include "app_stack_monitor.h"
#include "bme_sensor_service.h"
#include "c5_backpressure_controller.h"
#include "c5_memory.h"
#include "csi_service.h"
#include "gateway_link.h"
#include "lcd_service.h"
#include "speaker_player.h"
#include "system_service.h"
#include "voice_chain.h"
#include "wifi_manager.h"

/* 日志标签：只在本文件使用，不作为调试参数。 */
static const char *TAG = "APP_MAIN";

static lcd_dashboard_air_state_t app_lcd_air_state(bme_sensor_air_state_t state)
{
    switch (state) {
    case BME_SENSOR_AIR_STATE_READY:
        return LCD_DASHBOARD_AIR_READY;
    case BME_SENSOR_AIR_STATE_DEGRADED:
        return LCD_DASHBOARD_AIR_DEGRADED;
    case BME_SENSOR_AIR_STATE_CALIBRATING:
        return LCD_DASHBOARD_AIR_CALIBRATING;
    case BME_SENSOR_AIR_STATE_INIT:
    default:
        return LCD_DASHBOARD_AIR_INIT;
    }
}

static lcd_dashboard_motion_state_t app_lcd_motion_state(csi_service_motion_state_t state)
{
    switch (state) {
    case CSI_SERVICE_MOTION_IDLE:
        return LCD_DASHBOARD_MOTION_IDLE;
    case CSI_SERVICE_MOTION_MOTION:
        return LCD_DASHBOARD_MOTION_MOTION;
    case CSI_SERVICE_MOTION_INIT:
    default:
        return LCD_DASHBOARD_MOTION_INIT;
    }
}

static lcd_dashboard_voice_state_t app_lcd_voice_state(voice_chain_state_t state)
{
    switch (state) {
    case VOICE_WAKE_ACK:
        return LCD_DASHBOARD_VOICE_WAKE;
    case VOICE_RECORDING:
        return LCD_DASHBOARD_VOICE_REC;
    case VOICE_WAITING_RESPONSE:
        return LCD_DASHBOARD_VOICE_WAIT;
    case VOICE_PLAYING:
        return LCD_DASHBOARD_VOICE_PLAY;
    case VOICE_ERROR:
        return LCD_DASHBOARD_VOICE_ERR;
    case VOICE_IDLE:
    case VOICE_LISTENING:
    default:
        return LCD_DASHBOARD_VOICE_LISTEN;
    }
}

/* Called by the LVGL task. Each source call takes a short local snapshot only. */
static bool app_lcd_dashboard_snapshot(lcd_dashboard_snapshot_t *out_snapshot, void *user_ctx)
{
    (void)user_ctx;
    if (out_snapshot == NULL) {
        return false;
    }

    bme_sensor_snapshot_t bme = {0};
    csi_service_snapshot_t csi = {0};
    bme_sensor_service_get_latest_snapshot(&bme);
    csi_service_get_latest_snapshot(&csi);

    *out_snapshot = (lcd_dashboard_snapshot_t){
        .bme_valid = bme.valid,
        .temperature_c = bme.temperature_c,
        .humidity_percent = bme.humidity_percent,
        .pressure_hpa = bme.pressure_hpa,
        .gas_resistance_ohm = bme.gas_resistance_ohm,
        .gas_valid = bme.gas_valid,
        .air_state = app_lcd_air_state(bme.air_state),
        .csi_valid = csi.valid,
        .motion_score = csi.motion_score,
        .confidence = csi.confidence,
        .motion_state = app_lcd_motion_state(csi.motion_state),
        .network_ok = gateway_link_wifi_is_stable(),
        .gateway_ok = gateway_link_is_ready(),
        .voice_state = app_lcd_voice_state(voice_chain_get_state()),
        .speaker_active = audio_player_is_speaker_active(),
    };
    return true;
}

void app_orchestrator_start(void)
{
    char connected_ssid[33] = {0};

    app_stack_monitor_log(TAG, "app_startup_task", "orchestrator_enter");
    c5_mem_log("startup");

    /* Start the local dashboard before any network readiness gate. Its provider
     * reads published snapshots only, so unavailable services render their
     * initial state until they come online. */
    lcd_service_set_snapshot_provider(app_lcd_dashboard_snapshot, NULL);
    ESP_LOGI(TAG, "LCD_START_BEGIN");
    esp_err_t lcd_ret = lcd_service_start();
    if (lcd_ret != ESP_OK) {
        ESP_LOGW(TAG, "LCD/LVGL service start failed: %s", esp_err_to_name(lcd_ret));
    } else {
        ESP_LOGI(TAG, "LCD_START_DONE");
    }
    app_stack_monitor_log(TAG, "app_startup_task", "after_lcd_service_start");

    /*
     * 启动流程边界：
     * 1. WiFi 只连接 S3 SoftAP；
     * 2. system_service 通过 /local/v1/register/heartbeat/status/commands 与 S3 交互；
     * 3. BME 通过轻量 local JSON 上报；
     * 4. voice_chain 通过 /local/v1/voice/turn 发送 PCM，S3 再代理完整 Server 协议。
     */

    // 初始化 WiFi 管理器：内部完成 NVS、网络接口、事件循环和 STA 模式初始化。
    ESP_ERROR_CHECK(wifi_manager_init());
    app_stack_monitor_log(TAG, "app_startup_task", "after_wifi_manager_init");

    ESP_ERROR_CHECK(gateway_link_start());
    app_stack_monitor_log(TAG, "app_startup_task", "after_gateway_link_start");

    /* These services only register local state and workers here. Network work
     * remains gated by gateway readiness inside the runtime controller. */
    esp_err_t system_ret = system_service_init();
    if (system_ret != ESP_OK) {
        ESP_LOGW(TAG, "Local gateway system service init failed: %s", esp_err_to_name(system_ret));
    }
    app_stack_monitor_log(TAG, "app_startup_task", "after_system_service_init");

    if (MAIN_ENABLE_BME_SERVICE) {
        esp_err_t bme_ret = bme_sensor_service_start();
        if (bme_ret != ESP_OK) {
            ESP_LOGE(TAG, "BME service start failed: %s", esp_err_to_name(bme_ret));
        } else {
            c5_mem_log("after_bme_start");
        }
    } else {
        ESP_LOGI(TAG, "BME service disabled by MAIN_ENABLE_BME_SERVICE");
    }
    app_stack_monitor_log(TAG, "app_startup_task", "after_bme_service_start");

    esp_err_t scheduler_ret = c5_scheduler_start();
    if (scheduler_ret != ESP_OK) {
        ESP_LOGE(TAG, "C5 runtime dispatcher start failed: %s", esp_err_to_name(scheduler_ret));
    }
    app_stack_monitor_log(TAG, "app_startup_task", "after_c5_scheduler_start");

    // C5 terminal 正式模式只连接 ESPS3 SoftAP，不连接家庭 WiFi。
    if (wifi_connect_to_ap() != ESP_OK) {
        ESP_LOGE(TAG, "WiFi connect failed");
        while (1) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    if (wifi_get_connected_ssid(connected_ssid, sizeof(connected_ssid))) {
        ESP_LOGI(TAG, "WiFi connected: %s", connected_ssid);
    } else {
        ESP_LOGI(TAG, "WiFi connected");
    }
    app_stack_monitor_log(TAG, "app_startup_task", "after_wifi_connect");
    c5_mem_log("after_wifi_connect");

    // 等待 WiFi 连续稳定后再探测 S3 local HTTP，避免刚拿到 IP 时就发起业务请求。
    while (!wifi_is_stable()) {
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    app_stack_monitor_log(TAG, "app_startup_task", "after_wifi_stable");

    /*
     * C5 与 S3 断联时，恢复 S3 连接是最高优先级。只有 health probe 和 child register
     * 成功进入 LINK_READY 后，才允许 heartbeat/status/command、BME 上传和 server voice。
     */
    ESP_ERROR_CHECK(gateway_link_wait_ready(GATEWAY_LINK_WAIT_FOREVER_MS));
    app_stack_monitor_log(TAG, "app_startup_task", "after_gateway_link_ready");
    ESP_LOGI(TAG,
             "heap summary: free=%u min_free=%u largest_8bit_block=%u",
             (unsigned int)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned int)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT),
             (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

#if MAIN_ENABLE_CSI_SERVICE
    /*
     * CSI 运行链路必须显式打开。默认构建不会编译到这里的 init/start 调用，
     * 避免 C5 在主启动链路中注册 WiFi CSI callback 或上传 CSI 结果。
     */
    esp_err_t csi_ret = csi_service_init();
    if (csi_ret == ESP_OK) {
        csi_ret = csi_service_start();
    }
    if (csi_ret != ESP_OK) {
        ESP_LOGW(TAG, "CSI service start skipped: %s", esp_err_to_name(csi_ret));
    } else {
        c5_mem_log("after_csi_start");
    }
#else
    ESP_LOGI(TAG, "CSI service disabled by MAIN_ENABLE_CSI_SERVICE");
#endif
    app_stack_monitor_log(TAG, "app_startup_task", "after_csi_service_gate");

    if (MAIN_ENABLE_SPEAKER_SELF_TEST) {
        /*
         * Speaker 自检直接走 speaker_player/IIS，不经过 server voice。
         * 用于区分硬件/IIS/功放问题和服务器 PCM/播放路径问题。
         */
        esp_err_t speaker_test_ret =
            audio_player_self_test_1khz(MAIN_SPEAKER_SELF_TEST_DURATION_MS);
        if (speaker_test_ret != ESP_OK) {
            ESP_LOGE(TAG, "Speaker self-test failed: %s", esp_err_to_name(speaker_test_ret));
        } else {
            ESP_LOGI(TAG, "Speaker self-test done");
        }
        esp_err_t speaker_release_ret =
            audio_player_release_session(AUDIO_PLAYER_DRAIN_TIMEOUT_MS);
        if (speaker_release_ret != ESP_OK) {
            ESP_LOGW(TAG,
                     "Speaker self-test session release failed: %s",
                     esp_err_to_name(speaker_release_ret));
        }
    }
    app_stack_monitor_log(TAG, "app_startup_task", "after_speaker_self_test_gate");

    if (MAIN_ENABLE_MIC_CHAIN) {
        /*
         * WiFi 稳定后启动完整本地网关半双工语音链路：
         * Mic/VAD -> ESPS3 /local/v1/voice/turn -> speaker 播放 S3 回传 PCM -> 恢复 Mic。
         */
        esp_err_t voice_ret = voice_chain_start();
        if (voice_ret != ESP_OK) {
            ESP_LOGE(TAG, "Voice chain start failed: %s", esp_err_to_name(voice_ret));
        }
    } else {
        ESP_LOGI(TAG, "Mic chain disabled by MAIN_ENABLE_MIC_CHAIN");
    }
    app_stack_monitor_log(TAG, "app_startup_task", "after_voice_chain_start");

    // WiFi 重连、Mic ADC/VAD、本地 voice turn、speaker PCM 播放和 C5 runtime 都在后台任务中运行。
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(MAIN_IDLE_DELAY_MS));
    }
}
