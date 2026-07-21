#include "radar_service.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "app_stack_monitor.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

/*
 * 服务任务独占读取 UART、解析帧并更新存在快照。对外仅暴露副本，
 * S3 还将 UART 故障交给恢复状态机处理，避免接收任务在错误路径中忙等。
 */
#include "ld2450_config.h"
#include "radar_config.h"
#include "radar_uart_recovery.h"

static const char *TAG = "radar_ld2450";

static StaticSemaphore_t s_lock_storage;
static SemaphoreHandle_t s_lock;
static StaticSemaphore_t s_parser_lock_storage;
static SemaphoreHandle_t s_parser_lock;
static StaticSemaphore_t s_rx_buffer_lock_storage;
static SemaphoreHandle_t s_rx_buffer_lock;
static StaticSemaphore_t s_rx_exit_storage;
static SemaphoreHandle_t s_rx_exit;
static ld2450_parser_t s_parser;
static radar_presence_t s_presence;
static radar_uart_recovery_t s_recovery;
static radar_frame_t s_latest_frame;
static TaskHandle_t s_rx_task;
static bool s_initialized;
static volatile bool s_stop_requested;
static bool s_uart_healthy;
static bool s_config_active;
static bool s_has_latest_frame;
static bool s_uart_stop_applied;
static uint32_t s_snapshot_updates;
static uint64_t s_last_rx_byte_ms;
static uint8_t s_pending_rx_bytes[RADAR_CONFIG_UART_RX_RING_BYTES];
static uint8_t s_parser_feed_bytes[RADAR_CONFIG_UART_RX_RING_BYTES];
static size_t s_pending_rx_length;
static uint32_t s_pending_rx_drops;
static uint32_t s_processed_rx_bytes;

#if defined(CONFIG_RADAR_RAW_DEBUG) && CONFIG_RADAR_RAW_DEBUG
#define RADAR_ENABLE_RAW_HEX_LOG 1
#else
#define RADAR_ENABLE_RAW_HEX_LOG 0
#endif

#define RADAR_RAW_HEX_LOG_MAX_BYTES 256U
#define RADAR_RAW_HEX_LOG_INTERVAL_MS 1000U
#define RADAR_S3_LOG_IDENTITY_FORMAT \
    " source_id=0 source=S3_LOCAL device_id=sensair_s3_gateway_01 room=s3_local sequence=%lu"
#define RADAR_S3_LOG_IDENTITY_ARGS (unsigned long)s_snapshot_updates

static void handle_frame(const radar_frame_t *frame, void *ctx);

static bool uart_driver_admitted(const char *stage)
{
    const size_t free_bytes = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t largest_bytes =
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const bool admitted = free_bytes >= RADAR_CONFIG_UART_INTERNAL_MIN_FREE_BYTES &&
                          largest_bytes >= RADAR_CONFIG_UART_INTERNAL_MIN_LARGEST_BYTES;
    ESP_LOGI(TAG,
             "RADAR_MEMORY_ADMISSION stage=%s internal_free=%u internal_largest=%u admitted=%u",
             stage != NULL ? stage : "uart_driver",
             (unsigned int)free_bytes,
             (unsigned int)largest_bytes,
             admitted ? 1U : 0U);
    return admitted;
}

static esp_err_t uart_init_with_admission(const char *stage)
{
    return uart_driver_admitted(stage) ? ld2450_uart_init() : ESP_ERR_NO_MEM;
}

static uint64_t now_ms(void)
{
    int64_t now_us = esp_timer_get_time();
    return now_us > 0 ? (uint64_t)(now_us / 1000) : 0U;
}

#if RADAR_ENABLE_RAW_HEX_LOG
static void log_raw_rx_hex(const uint8_t *data, size_t data_len, uint64_t timestamp_ms)
{
    static uint64_t last_log_ms;
    if (data == NULL || data_len == 0U ||
        (last_log_ms != 0U && timestamp_ms - last_log_ms < RADAR_RAW_HEX_LOG_INTERVAL_MS)) {
        return;
    }

    const size_t dump_len = data_len > RADAR_RAW_HEX_LOG_MAX_BYTES
                                ? RADAR_RAW_HEX_LOG_MAX_BYTES
                                : data_len;
    ESP_LOGI(TAG,
             "RADAR_RX_FRAME event=raw_hex len=%u timestamp_ms=%llu dump_len=%u%s" RADAR_S3_LOG_IDENTITY_FORMAT,
             (unsigned int)data_len,
             (unsigned long long)timestamp_ms,
             (unsigned int)dump_len,
             data_len > dump_len ? " (truncated)" : "",
             RADAR_S3_LOG_IDENTITY_ARGS);

    for (size_t offset = 0U; offset < dump_len; offset += 16U) {
        char hex_line[16U * 3U];
        size_t written = 0U;
        const size_t line_len = (dump_len - offset) > 16U ? 16U : dump_len - offset;
        for (size_t index = 0U; index < line_len; ++index) {
            const int count = snprintf(hex_line + written,
                                       sizeof(hex_line) - written,
                                       index == 0U ? "%02x" : " %02x",
                                       data[offset + index]);
            if (count < 0 || (size_t)count >= sizeof(hex_line) - written) {
                break;
            }
            written += (size_t)count;
        }
        /* snprintf leaves no room for a terminator only on truncation, which
         * is handled above.  Keep the %s argument explicitly bounded. */
        hex_line[written] = '\0';
        ESP_LOGI(TAG,
                 "RADAR_RX_FRAME event=raw_hex_data bytes=%s" RADAR_S3_LOG_IDENTITY_FORMAT,
                 hex_line,
                 RADAR_S3_LOG_IDENTITY_ARGS);
    }
    last_log_ms = timestamp_ms;
}
#endif

static void sat_inc_u32(uint32_t *value)
{
    if (value != NULL && *value < UINT32_MAX) {
        ++(*value);
    }
}

static void parser_force_reset(void)
{
    if (s_parser_lock != NULL && xSemaphoreTake(s_parser_lock, portMAX_DELAY) == pdTRUE) {
        ld2450_parser_force_reset(&s_parser);
        xSemaphoreGive(s_parser_lock);
    }
}

static size_t parser_feed(const uint8_t *data,
                          size_t data_len,
                          uint64_t received_at_ms)
{
    if (s_parser_lock == NULL ||
        xSemaphoreTake(s_parser_lock, portMAX_DELAY) != pdTRUE) {
        return 0U;
    }
    const size_t published = ld2450_parser_feed(&s_parser,
                                                 data,
                                                 data_len,
                                                 received_at_ms,
                                                 handle_frame,
                                                 NULL);
    xSemaphoreGive(s_parser_lock);
    return published;
}

static void enqueue_rx_bytes(const uint8_t *data, size_t data_len)
{
    if (data == NULL || data_len == 0U || s_rx_buffer_lock == NULL ||
        xSemaphoreTake(s_rx_buffer_lock, portMAX_DELAY) != pdTRUE) {
        return;
    }
    const size_t capacity = sizeof(s_pending_rx_bytes);
    if (s_pending_rx_length > capacity) {
        ESP_LOGE(TAG,
                 "RADAR_MEMORY_CORRUPTION stage=rx_pending_length value=%u capacity=%u action=reset",
                 (unsigned int)s_pending_rx_length,
                 (unsigned int)capacity);
        s_pending_rx_length = 0U;
        sat_inc_u32(&s_pending_rx_drops);
    }
    const size_t available = capacity - s_pending_rx_length;
    const size_t copied = data_len < available ? data_len : available;
    if (copied > 0U) {
        memcpy(s_pending_rx_bytes + s_pending_rx_length, data, copied);
        s_pending_rx_length += copied;
    }
    if (copied < data_len && s_pending_rx_drops < UINT32_MAX) {
        ++s_pending_rx_drops;
    }
    xSemaphoreGive(s_rx_buffer_lock);
}

size_t radar_service_process_pending(uint64_t processed_at_ms)
{
    if (s_rx_buffer_lock == NULL ||
        xSemaphoreTake(s_rx_buffer_lock, portMAX_DELAY) != pdTRUE) {
        return 0U;
    }
    const size_t length = s_pending_rx_length > sizeof(s_parser_feed_bytes)
        ? sizeof(s_parser_feed_bytes) : s_pending_rx_length;
    if (s_pending_rx_length > sizeof(s_parser_feed_bytes)) {
        ESP_LOGE(TAG,
                 "RADAR_MEMORY_CORRUPTION stage=rx_pending_copy value=%u capacity=%u action=clamp",
                 (unsigned int)s_pending_rx_length,
                 (unsigned int)sizeof(s_parser_feed_bytes));
    }
    if (length > 0U) {
        memcpy(s_parser_feed_bytes, s_pending_rx_bytes, length);
        s_pending_rx_length = 0U;
    }
    xSemaphoreGive(s_rx_buffer_lock);
    if (length == 0U) {
        return 0U;
    }
    const size_t published = parser_feed(s_parser_feed_bytes, length, processed_at_ms);
    if (s_lock != NULL && xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
        const uint64_t total = (uint64_t)s_processed_rx_bytes + length;
        s_processed_rx_bytes = total > UINT32_MAX ? UINT32_MAX : (uint32_t)total;
        xSemaphoreGive(s_lock);
    }
    return published;
}

static void parser_note_timeout(uint64_t timestamp_ms)
{
    if (s_parser_lock != NULL && xSemaphoreTake(s_parser_lock, portMAX_DELAY) == pdTRUE) {
        ld2450_parser_note_timeout(&s_parser, timestamp_ms);
        xSemaphoreGive(s_parser_lock);
    }
}

static void parser_get_diagnostics(ld2450_parser_diagnostics_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    if (s_parser_lock != NULL && xSemaphoreTake(s_parser_lock, portMAX_DELAY) == pdTRUE) {
        ld2450_parser_get_diagnostics(&s_parser, out);
        xSemaphoreGive(s_parser_lock);
    }
}

static void handle_frame(const radar_frame_t *frame, void *ctx)
{
    (void)ctx;
    if (frame == NULL || s_lock == NULL) {
        return;
    }
    if (xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
        radar_uart_recovery_note_valid_frame(&s_recovery, frame->received_at_ms);
        s_uart_healthy = s_recovery.state == RADAR_UART_RECOVERY_VALID;
        s_latest_frame = *frame;
        s_has_latest_frame = true;
        sat_inc_u32(&s_snapshot_updates);
        xSemaphoreGive(s_lock);
    }
}

static void apply_recovery_transition(uint64_t timestamp_ms)
{
    if (s_lock == NULL || xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
        return;
    }
    const bool stop_rx = radar_uart_recovery_should_stop_rx(&s_recovery);
    if (stop_rx && !s_uart_stop_applied) {
        s_uart_healthy = false;
        radar_presence_note_uart_error(&s_presence, timestamp_ms);
        s_uart_stop_applied = true;
        xSemaphoreGive(s_lock);
        ld2450_parser_diagnostics_t diagnostics;
        parser_get_diagnostics(&diagnostics);
        if (xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
            const uint64_t last_rx_byte_ms = s_last_rx_byte_ms;
            radar_uart_recovery_record_snapshot(&s_recovery,
                                                diagnostics.partial_length,
                                                diagnostics.skipped_bytes,
                                                last_rx_byte_ms);
            xSemaphoreGive(s_lock);
            ESP_LOGW(TAG,
                     "RADAR_SOURCE_STATE event=uart_recovery_snapshot partial=%lu skipped=%lu last_rx_ms=%llu" RADAR_S3_LOG_IDENTITY_FORMAT,
                     (unsigned long)diagnostics.partial_length,
                     (unsigned long)diagnostics.skipped_bytes,
                     (unsigned long long)last_rx_byte_ms,
                     RADAR_S3_LOG_IDENTITY_ARGS);
        }
        const esp_err_t flush_ret = ld2450_uart_flush();
        const esp_err_t ret = ld2450_uart_deinit();
        if (flush_ret != ESP_OK) {
            ESP_LOGW(TAG,
                     "RADAR_SOURCE_STATE event=uart_recovery_flush_failed ret=%d" RADAR_S3_LOG_IDENTITY_FORMAT,
                     (int)flush_ret,
                     RADAR_S3_LOG_IDENTITY_ARGS);
        }
        if (ret != ESP_OK) {
            ESP_LOGW(TAG,
                     "RADAR_SOURCE_STATE event=uart_recovery_deinit_failed ret=%d backoff=1" RADAR_S3_LOG_IDENTITY_FORMAT,
                     (int)ret,
                     RADAR_S3_LOG_IDENTITY_ARGS);
            if (xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
                radar_uart_recovery_note_init_result(&s_recovery, false, timestamp_ms);
                s_uart_healthy = false;
                s_uart_stop_applied = true;
                xSemaphoreGive(s_lock);
            }
        }
        return;
    }
    xSemaphoreGive(s_lock);
}

static void retry_uart_if_due(uint64_t timestamp_ms)
{
    if (s_lock == NULL || xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
        return;
    }
    const bool due = radar_uart_recovery_retry_due(&s_recovery, timestamp_ms);
    xSemaphoreGive(s_lock);
    if (!due) {
        return;
    }

    /* Retry a failed delete before reconfiguring so init cannot accept a stale driver. */
    esp_err_t ret = ld2450_uart_deinit();
    if (ret == ESP_OK) {
        ret = uart_init_with_admission("uart_recovery_driver");
    }
    /* Recovery owns the only explicit parser reset path. */
    parser_force_reset();
    if (xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
        radar_uart_recovery_note_init_result(&s_recovery, ret == ESP_OK, timestamp_ms);
        s_uart_healthy = false;
        s_uart_stop_applied = ret != ESP_OK;
        xSemaphoreGive(s_lock);
    }
    if (ret == ESP_OK) {
        ESP_LOGI(TAG,
                 "RADAR_SOURCE_STATE event=uart_recovery_reinitialized awaiting_valid_frames=1" RADAR_S3_LOG_IDENTITY_FORMAT,
                 RADAR_S3_LOG_IDENTITY_ARGS);
    } else {
        ESP_LOGW(TAG,
                 "RADAR_SOURCE_STATE event=uart_recovery_init_failed ret=%d backoff=1" RADAR_S3_LOG_IDENTITY_FORMAT,
                 (int)ret,
                 RADAR_S3_LOG_IDENTITY_ARGS);
    }
}

static bool config_is_active(void)
{
    bool active = false;
    if (s_lock != NULL && xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
        active = s_config_active;
        xSemaphoreGive(s_lock);
    }
    return active;
}

static void radar_rx_task(void *arg)
{
    (void)arg;
    uint8_t read_buffer[RADAR_CONFIG_UART_READ_BYTES];

    ESP_LOGI(TAG,
             "RADAR_SOURCE_STATE event=uart_config baud=%d rx_buf=%d timeout_ms=%u" RADAR_S3_LOG_IDENTITY_FORMAT,
             RADAR_CONFIG_UART_BAUD_RATE,
             RADAR_CONFIG_UART_RX_RING_BYTES,
             (unsigned int)RADAR_CONFIG_UART_READ_TIMEOUT_MS,
             RADAR_S3_LOG_IDENTITY_ARGS);
    ESP_LOGI(TAG,
             "RADAR_SOURCE_STATE event=rx_task_started read_bytes=%u timeout_ms=%u" RADAR_S3_LOG_IDENTITY_FORMAT,
             (unsigned int)sizeof(read_buffer),
             (unsigned int)RADAR_CONFIG_UART_READ_TIMEOUT_MS,
             RADAR_S3_LOG_IDENTITY_ARGS);

    while (!s_stop_requested) {
        if (config_is_active()) {
            vTaskDelay(pdMS_TO_TICKS(RADAR_CONFIG_UART_IDLE_DELAY_MS));
            continue;
        }

        const uint64_t timestamp_ms = now_ms();
        if (s_lock != NULL && xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
            const bool backoff = radar_uart_recovery_should_stop_rx(&s_recovery);
            const uint32_t delay_ms = radar_uart_recovery_delay_ms(&s_recovery, timestamp_ms);
            xSemaphoreGive(s_lock);
            if (backoff) {
                retry_uart_if_due(timestamp_ms);
                if (delay_ms > 0U) {
                    vTaskDelay(pdMS_TO_TICKS(delay_ms > RADAR_CONFIG_UART_RECOVERY_DELAY_CAP_MS
                                                 ? RADAR_CONFIG_UART_RECOVERY_DELAY_CAP_MS
                                                 : delay_ms));
                }
                continue;
            }
        }

        ld2450_uart_events_t events;
        ld2450_uart_drain_events(&events);
        if (events.overflow || events.buffer_full || events.line_error) {
            if (xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
                if (events.overflow || events.buffer_full) {
                    radar_uart_recovery_note_overflow(&s_recovery, timestamp_ms);
                }
                if (events.line_error) {
                    radar_uart_recovery_note_error(&s_recovery, timestamp_ms);
                }
                xSemaphoreGive(s_lock);
            }
            apply_recovery_transition(timestamp_ms);
            continue;
        }

        int read_len = ld2450_uart_read(read_buffer, sizeof(read_buffer),
                                        RADAR_CONFIG_UART_READ_TIMEOUT_MS);
        if (read_len > 0) {
            if (xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
                s_last_rx_byte_ms = timestamp_ms;
                radar_uart_recovery_note_rx_bytes(&s_recovery,
                                                  timestamp_ms,
                                                  (uint32_t)read_len);
                xSemaphoreGive(s_lock);
            }
#if RADAR_ENABLE_RAW_HEX_LOG
            log_raw_rx_hex(read_buffer, (size_t)read_len, timestamp_ms);
#endif
            enqueue_rx_bytes(read_buffer, (size_t)read_len);
        } else if (read_len == 0) {
            parser_note_timeout(timestamp_ms);
            if (xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
                radar_uart_recovery_note_timeout(&s_recovery, timestamp_ms);
                xSemaphoreGive(s_lock);
            }
        } else {
            if (xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
                radar_uart_recovery_note_error(&s_recovery, timestamp_ms);
                xSemaphoreGive(s_lock);
            }
        }
        apply_recovery_transition(timestamp_ms);
    }

    (void)ld2450_uart_deinit();
    if (s_lock != NULL && xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
        s_uart_healthy = false;
        s_uart_stop_applied = true;
        s_rx_task = NULL;
        xSemaphoreGive(s_lock);
    } else {
        s_rx_task = NULL;
    }
    if (s_rx_exit != NULL) {
        xSemaphoreGive(s_rx_exit);
    }
    vTaskDelete(NULL);
}

esp_err_t radar_service_init(const radar_presence_config_t *config)
{
    if (s_initialized) {
        return ESP_OK;
    }

    s_lock = xSemaphoreCreateMutexStatic(&s_lock_storage);
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s_parser_lock = xSemaphoreCreateMutexStatic(&s_parser_lock_storage);
    if (s_parser_lock == NULL) {
        s_lock = NULL;
        return ESP_ERR_NO_MEM;
    }
    s_rx_buffer_lock = xSemaphoreCreateMutexStatic(&s_rx_buffer_lock_storage);
    if (s_rx_buffer_lock == NULL) {
        s_lock = NULL;
        s_parser_lock = NULL;
        return ESP_ERR_NO_MEM;
    }
    s_rx_exit = xSemaphoreCreateBinaryStatic(&s_rx_exit_storage);
    if (s_rx_exit == NULL) {
        s_lock = NULL;
        s_parser_lock = NULL;
        s_rx_buffer_lock = NULL;
        return ESP_ERR_NO_MEM;
    }
    ld2450_parser_init(&s_parser);
    radar_presence_init(&s_presence, config, now_ms());
    radar_uart_recovery_init(&s_recovery, NULL, now_ms());
    s_initialized = true;
    return ESP_OK;
}

esp_err_t radar_service_start(void)
{
    esp_err_t ret = radar_service_init(NULL);
    if (ret != ESP_OK) {
        return ret;
    }
    if (s_rx_task != NULL) {
        return ESP_OK;
    }

    while (xSemaphoreTake(s_rx_exit, 0) == pdTRUE) {
    }
    s_stop_requested = false;

    ret = uart_init_with_admission("uart_initial_driver");
    if (xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
        radar_uart_recovery_note_init_result(&s_recovery, ret == ESP_OK, now_ms());
        s_uart_healthy = false;
        s_uart_stop_applied = ret != ESP_OK;
        xSemaphoreGive(s_lock);
    }

    BaseType_t created = xTaskCreateWithCaps(radar_rx_task,
                                             "radar_rx",
                                             RADAR_CONFIG_UART_RX_TASK_STACK,
                                             NULL,
                                             RADAR_CONFIG_UART_RX_TASK_PRIORITY,
                                             &s_rx_task,
                                             MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (created != pdPASS) {
        s_rx_task = NULL;
        s_stop_requested = false;
        (void)ld2450_uart_deinit();
        return ESP_ERR_NO_MEM;
    }
    app_stack_monitor_log_task_created(TAG,
                                       "radar_rx",
                                       s_rx_task,
                                       RADAR_CONFIG_UART_RX_TASK_STACK);

    ESP_LOGI(TAG,
             "RADAR_SOURCE_STATE event=uart_init port=%d tx=%d rx=%d baud=%d format=8N1 ret=%d ring=%d" RADAR_S3_LOG_IDENTITY_FORMAT,
             RADAR_CONFIG_UART_PORT_INDEX,
             RADAR_CONFIG_UART_TX_GPIO,
             RADAR_CONFIG_UART_RX_GPIO,
             RADAR_CONFIG_UART_BAUD_RATE,
             (int)ret,
             RADAR_CONFIG_UART_RX_RING_BYTES,
             RADAR_S3_LOG_IDENTITY_ARGS);
    return ret == ESP_OK ? ESP_OK : ESP_ERR_INVALID_STATE;
}

esp_err_t radar_service_stop(void)
{
    TaskHandle_t task = s_rx_task;
    if (task == NULL) {
        s_stop_requested = false;
        return ld2450_uart_deinit();
    }
    if (task == xTaskGetCurrentTaskHandle()) {
        return ESP_ERR_INVALID_STATE;
    }

    s_stop_requested = true;
    if (s_rx_exit == NULL ||
        xSemaphoreTake(s_rx_exit, pdMS_TO_TICKS(RADAR_CONFIG_TASK_STOP_TIMEOUT_MS)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    s_stop_requested = false;
    return ESP_OK;
}

bool radar_service_get_latest_frame(radar_frame_t *out)
{
    if (out == NULL || s_lock == NULL || xSemaphoreTake(s_lock, portMAX_DELAY) != pdTRUE) {
        return false;
    }
    const bool available = s_has_latest_frame;
    if (available) {
        *out = s_latest_frame;
    }
    xSemaphoreGive(s_lock);
    return available;
}

bool radar_service_is_started(void)
{
    return s_rx_task != NULL;
}

void radar_service_get_snapshot(radar_snapshot_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->state = RADAR_STATE_UNKNOWN;
    if (s_lock != NULL && xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
        radar_presence_get_snapshot(&s_presence, out);
        xSemaphoreGive(s_lock);
    }
}

void radar_service_get_diagnostics(radar_service_diagnostics_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    parser_get_diagnostics(&out->parser);
    ld2450_uart_get_diagnostics(&out->uart);
    if (s_lock != NULL && xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE) {
        radar_presence_get_diagnostics(&s_presence, &out->presence);
        out->recovery = s_recovery;
        out->snapshot_updates = s_snapshot_updates;
        out->processed_rx_bytes = s_processed_rx_bytes;
        xSemaphoreGive(s_lock);
    }
    if (s_rx_buffer_lock != NULL &&
        xSemaphoreTake(s_rx_buffer_lock, portMAX_DELAY) == pdTRUE) {
        out->pending_rx_bytes = s_pending_rx_length > UINT32_MAX ? UINT32_MAX :
                                (uint32_t)s_pending_rx_length;
        out->pending_rx_drops = s_pending_rx_drops;
        xSemaphoreGive(s_rx_buffer_lock);
    }
}
