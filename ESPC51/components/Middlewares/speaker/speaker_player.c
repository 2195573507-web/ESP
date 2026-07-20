#include "speaker_player.h"

/**
 * @file speaker_player.c
 * @brief speaker PCM 播放器实现。
 *
 * 播放路径：
 * 1. 上层传入 PCM16 mono buffer。
 * 2. 本模块把 PCM 拆成固定 512 个采样点的音频块，放入 FreeRTOS 环形缓冲区。
 * 3. 写入任务独占调用 iis_write()，避免多个任务同时触碰 IIS/PDM DMA。
 * 4. 播放结束后发送 END 条目，统一停止 IIS 并释放本次播放资源。
 */

#include <stdbool.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "app_debug_config.h"
#include "app_stack_monitor.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "c5_memory.h"
#include "iis.h"

static const char *TAG = "speaker_player";

static void speaker_player_log_writer_task_alloc_check(void)
{
    ESP_LOGI(TAG,
             "SPEAKER_IIS_WRITER_TASK_ALLOC_CHECK internal_free=%u internal_largest=%u psram_free=%u stack_size=%u source=internal",
             (unsigned int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
             (unsigned int)AUDIO_PLAYER_I2S_WRITER_TASK_STACK_SIZE);
}

/* 环形缓冲区内部条目类型，仅供本实现的 writer task 识别。 */
#define AUDIO_PLAYER_RING_ITEM_TYPE_PCM 1U
#define AUDIO_PLAYER_RING_ITEM_TYPE_END 2U
#define AUDIO_PLAYER_RING_ITEM_TYPE_START 3U

#ifndef AUDIO_PLAYER_ABORT_WAIT_MS
#define AUDIO_PLAYER_ABORT_WAIT_MS 2000U // 断联中止播放时等待 writer 退出的保护超时。
#endif

#ifndef AUDIO_PLAYER_ABORT_POLL_MS
#define AUDIO_PLAYER_ABORT_POLL_MS 50U // writer 等待 ringbuffer 时检查 abort 的周期。
#endif

/* 播放互斥锁：同一时刻只允许一个 PCM 流占用 IIS/PDM 输出。 */
static SemaphoreHandle_t s_play_mutex = NULL;
static StaticSemaphore_t s_play_mutex_storage;
static StaticSemaphore_t s_ring_items_storage;
static StaticSemaphore_t s_ring_space_storage;
static StaticSemaphore_t s_stream_done_storage;
static StaticSemaphore_t s_stream_started_storage;
#define AUDIO_PLAYER_I2S_WRITER_STACK_WORDS \
    ((AUDIO_PLAYER_I2S_WRITER_TASK_STACK_SIZE + sizeof(StackType_t) - 1U) / sizeof(StackType_t))
static StackType_t *s_writer_task_stack;
static StaticTask_t s_writer_task_storage;
static portMUX_TYPE s_ring_lock = portMUX_INITIALIZER_UNLOCKED;
static int16_t s_self_test_chunk[AUDIO_PLAYER_PCM_CHUNK_SAMPLES];

/* 播放期间可选 heap 周期诊断，默认由 APP_DEBUG_SPEAKER_PLAYER_LOG 关闭。 */
static esp_timer_handle_t s_heap_monitor_timer = NULL;

/* 记录每次写 DMA 的耗时，用于定位 IIS DMA 饥饿或阻塞。 */
typedef struct {
    uint32_t write_count;
    uint64_t total_block_us;
    int64_t max_block_us;
    uint64_t total_requested_bytes;
    uint64_t total_written_bytes;
    uint64_t total_valid_samples;
    uint64_t total_dma_samples;
    uint32_t pcm_chunk_count;
    uint32_t short_write_count;
    uint32_t zero_write_count;
} audio_player_dma_diag_t;

static audio_player_dma_diag_t s_dma_diag = {0};

/* 环形缓冲区中传递的固定大小条目。最后一个不足 512 个采样点的音频块会补 0。 */
typedef struct {
    uint32_t type;
    uint32_t generation;
    uint32_t sequence;
    uint32_t valid_samples;
    int16_t samples[AUDIO_PLAYER_PCM_CHUNK_SAMPLES];
} audio_player_ring_item_t;

typedef enum {
    AUDIO_PLAYER_STATE_UNINITIALIZED = 0,
    AUDIO_PLAYER_STATE_READY_DISABLED,
    AUDIO_PLAYER_STATE_OPENING,
    AUDIO_PLAYER_STATE_PLAYING,
    AUDIO_PLAYER_STATE_DRAINING,
    AUDIO_PLAYER_STATE_ABORTING,
} audio_player_state_t;

/* 播放资源常驻于 C5 生命周期；每一轮只复位 generation 和统计项。 */
typedef struct {
    audio_player_ring_item_t *ring_slots;
    uint32_t ring_capacity;
    uint32_t ring_read_index;
    uint32_t ring_write_index;
    uint32_t ring_used;
    SemaphoreHandle_t ring_items;
    SemaphoreHandle_t ring_space;
    SemaphoreHandle_t done;
    SemaphoreHandle_t started;
    volatile bool writer_done;
    volatile bool abort_requested;
    volatile bool writer_start_done;
    volatile uint32_t generation;
    esp_err_t result;
    esp_err_t writer_start_result;
    UBaseType_t writer_stack_high_water_bytes;
    audio_player_ring_item_t *scratch_item;
    uint8_t *dma_staging;
    TaskHandle_t writer_task;
    uint64_t valid_samples_written;
    uint64_t dma_samples_written;
    uint64_t dma_bytes_written;
    uint32_t pcm_chunks_written;
    uint32_t ring_high_water;
    uint32_t ring_backpressure_count;
} audio_player_stream_ctx_t;

typedef struct {
    volatile uint32_t callback_calls;
    volatile uint32_t deferred_notify_count;
    volatile uint32_t illegal_context_count;
    volatile uint32_t writer_start_notify_count;
    volatile uint32_t writer_end_notify_count;
    uint32_t logged_generation;
} audio_player_deferred_diag_t;

static audio_player_stream_ctx_t s_pcm_stream_ctx = {0};
static audio_player_deferred_diag_t s_deferred_diag = {0};
static bool s_pcm_stream_open;
static TaskHandle_t s_pcm_stream_owner_task;
static uint32_t s_pcm_stream_sequence;
static audio_player_state_t s_pcm_player_state = AUDIO_PLAYER_STATE_UNINITIALIZED;

static bool speaker_player_called_from_isr(void)
{
    if (!xPortInIsrContext()) {
        return false;
    }

    __atomic_fetch_add(&s_deferred_diag.illegal_context_count, 1U, __ATOMIC_RELAXED);
    return true;
}

static void speaker_player_log_deferred_diag(const audio_player_stream_ctx_t *ctx)
{
    if (ctx == NULL || s_deferred_diag.logged_generation == ctx->generation) {
        return;
    }

    s_deferred_diag.logged_generation = ctx->generation;
    ESP_LOGI(TAG,
             "SPEAKER_DEFERRED_DIAG generation=%lu callback_calls=%lu deferred_notifies=%lu "
             "illegal_context=%lu writer_start_notifies=%lu writer_end_notifies=%lu",
             (unsigned long)ctx->generation,
             (unsigned long)__atomic_load_n(&s_deferred_diag.callback_calls, __ATOMIC_RELAXED),
             (unsigned long)__atomic_load_n(&s_deferred_diag.deferred_notify_count, __ATOMIC_RELAXED),
             (unsigned long)__atomic_load_n(&s_deferred_diag.illegal_context_count, __ATOMIC_RELAXED),
             (unsigned long)__atomic_load_n(&s_deferred_diag.writer_start_notify_count, __ATOMIC_RELAXED),
             (unsigned long)__atomic_load_n(&s_deferred_diag.writer_end_notify_count, __ATOMIC_RELAXED));
}

static void speaker_player_log_values(const char *message,
                                      int64_t value0,
                                      int64_t value1,
                                      int64_t value2,
                                      int64_t value3)
{
#if APP_DEBUG_SPEAKER_PLAYER_LOG
    ESP_LOGI(TAG, "%s: %lld, %lld, %lld, %lld",
             message,
             value0,
             value1,
             value2,
             value3);
#else
    (void)message;
    (void)value0;
    (void)value1;
    (void)value2;
    (void)value3;
#endif
}

static void speaker_player_log_heap_state(const char *stage)
{
    speaker_player_log_values(stage,
                              esp_get_free_heap_size(),
                              (int64_t)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
                              0,
                              0);
}

#if ENABLE_VERBOSE_AUDIO_LOG
static UBaseType_t speaker_player_current_stack_high_water(void)
{
    return app_stack_monitor_high_water();
}
#endif

static UBaseType_t speaker_player_writer_stack_high_water(void)
{
#if (INCLUDE_uxTaskGetStackHighWaterMark == 1)
    return s_pcm_stream_ctx.writer_task == NULL ? 0 :
           uxTaskGetStackHighWaterMark(s_pcm_stream_ctx.writer_task) * sizeof(StackType_t);
#else
    return 0;
#endif
}

static void speaker_player_heap_monitor_timer_cb(void *arg)
{
    (void)arg;
    speaker_player_log_heap_state("play_running");
}

static esp_err_t speaker_player_heap_monitor_ensure_timer(void)
{
    if (s_heap_monitor_timer != NULL) {
        return ESP_OK;
    }

    const esp_timer_create_args_t timer_args = {
        .callback = speaker_player_heap_monitor_timer_cb,
        .name = "speaker_heap_monitor",
        .skip_unhandled_events = true,
    };
    return esp_timer_create(&timer_args, &s_heap_monitor_timer);
}

static void speaker_player_heap_monitor_start(void)
{
    speaker_player_log_heap_state("play_start");
    if (speaker_player_heap_monitor_ensure_timer() != ESP_OK) {
        return;
    }
    if (esp_timer_is_active(s_heap_monitor_timer)) {
        (void)esp_timer_stop(s_heap_monitor_timer);
    }
    (void)esp_timer_start_periodic(s_heap_monitor_timer,
                                   AUDIO_PLAYER_HEAP_MONITOR_INTERVAL_US);
}

static void speaker_player_heap_monitor_stop(void)
{
    if (s_heap_monitor_timer != NULL && esp_timer_is_active(s_heap_monitor_timer)) {
        (void)esp_timer_stop(s_heap_monitor_timer);
    }
}

static void speaker_player_dma_diag_reset(void)
{
    s_dma_diag.write_count = 0;
    s_dma_diag.total_block_us = 0;
    s_dma_diag.max_block_us = 0;
    s_dma_diag.total_requested_bytes = 0;
    s_dma_diag.total_written_bytes = 0;
    s_dma_diag.total_valid_samples = 0;
    s_dma_diag.total_dma_samples = 0;
    s_dma_diag.pcm_chunk_count = 0;
    s_dma_diag.short_write_count = 0;
    s_dma_diag.zero_write_count = 0;
}

static void speaker_player_dma_diag_record(size_t request_bytes,
                                           size_t written_bytes,
                                           int64_t elapsed_us)
{
    s_dma_diag.write_count++;
    s_dma_diag.total_block_us += (uint64_t)elapsed_us;
    s_dma_diag.total_requested_bytes += request_bytes;
    s_dma_diag.total_written_bytes += written_bytes;
    if (elapsed_us > s_dma_diag.max_block_us) {
        s_dma_diag.max_block_us = elapsed_us;
    }
    if (written_bytes == 0) {
        s_dma_diag.zero_write_count++;
    } else if (written_bytes < request_bytes) {
        s_dma_diag.short_write_count++;
    }

    if (elapsed_us > AUDIO_PLAYER_DMA_STARVATION_THRESHOLD_US) {
        ESP_LOGW(TAG,
                 "DMA starvation: request=%zu written=%zu elapsed_us=%lld",
                 request_bytes,
                 written_bytes,
                 elapsed_us);
    }
}

static void speaker_player_dma_diag_log_summary(void)
{
#if ENABLE_VERBOSE_AUDIO_LOG
    uint64_t avg_us = s_dma_diag.write_count == 0 ? 0 :
                      s_dma_diag.total_block_us / s_dma_diag.write_count;
    ESP_LOGI(TAG,
             "speaker DMA write summary calls=%lu requested_bytes=%llu written_bytes=%llu "
             "valid_samples=%llu dma_samples=%llu pcm_chunks=%lu short_writes=%lu "
             "zero_writes=%lu max_block_us=%lld avg_block_us=%llu",
             (unsigned long)s_dma_diag.write_count,
             (unsigned long long)s_dma_diag.total_requested_bytes,
             (unsigned long long)s_dma_diag.total_written_bytes,
             (unsigned long long)s_dma_diag.total_valid_samples,
             (unsigned long long)s_dma_diag.total_dma_samples,
             (unsigned long)s_dma_diag.pcm_chunk_count,
             (unsigned long)s_dma_diag.short_write_count,
             (unsigned long)s_dma_diag.zero_write_count,
             s_dma_diag.max_block_us,
             (unsigned long long)avg_us);
#endif
}

static esp_err_t speaker_player_ensure_mutex(void)
{
    if (s_play_mutex != NULL) {
        return ESP_OK;
    }
    s_play_mutex = xSemaphoreCreateMutexStatic(&s_play_mutex_storage);
    return s_play_mutex == NULL ? ESP_ERR_NO_MEM : ESP_OK;
}

static esp_err_t speaker_player_write_bytes_dma_streaming(audio_player_stream_ctx_t *ctx,
                                                          const void *data,
                                                          size_t total_bytes,
                                                          uint32_t valid_samples,
                                                          uint32_t sequence)
{
    /*
     * 写入任务使用非阻塞 iis_write() 轮询 DMA 可用空间。
     * 这样可以及时记录 DMA not ready，同时避免长时间阻塞导致上层无法收尾。
     */
    if (ctx == NULL || data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (total_bytes != AUDIO_PLAYER_PCM_CHUNK_BYTES) {
        ESP_LOGE(TAG,
                 "reject non-fixed IIS write: bytes=%zu expected=%zu",
                 total_bytes,
                 (size_t)AUDIO_PLAYER_PCM_CHUNK_BYTES);
        return ESP_ERR_INVALID_SIZE;
    }

    const uint8_t *bytes = (const uint8_t *)data;
    size_t offset = 0;
    uint32_t retry_count = 0;
    size_t chunk_written_bytes = 0;

    while (offset < total_bytes) {
        if (ctx->abort_requested) {
            return ESP_ERR_INVALID_STATE;
        }

        size_t bytes_written = 0;
        size_t bytes_left = total_bytes - offset;
        int64_t start_us = esp_timer_get_time();
        esp_err_t err = iis_write(bytes + offset,
                                  bytes_left,
                                  &bytes_written,
                                  AUDIO_PLAYER_I2S_NONBLOCK_TIMEOUT_MS);
        int64_t elapsed_us = esp_timer_get_time() - start_us;
        speaker_player_dma_diag_record(bytes_left, bytes_written, elapsed_us);

        if (bytes_written > 0) {
            offset += bytes_written;
            chunk_written_bytes += bytes_written;
            ctx->dma_bytes_written += bytes_written;
#if ENABLE_VERBOSE_AUDIO_LOG
            ESP_LOGI(TAG,
                     "speaker IIS write seq=%lu request_bytes=%zu bytes_written=%zu "
                     "chunk_written=%zu/%zu cumulative_dma_bytes=%llu elapsed_us=%lld",
                     (unsigned long)sequence,
                     bytes_left,
                     bytes_written,
                     chunk_written_bytes,
                     total_bytes,
                     (unsigned long long)ctx->dma_bytes_written,
                     elapsed_us);
#endif
        }

        if (err == ESP_OK && bytes_written > 0) {
            continue;
        }

        if (err == ESP_ERR_TIMEOUT || (err == ESP_OK && bytes_written == 0)) {
            retry_count++;
            if ((retry_count % AUDIO_PLAYER_I2S_RETRY_LOG_INTERVAL) == 0) {
                ESP_LOGW(TAG,
                         "IIS DMA not ready: retries=%lu offset=%zu total=%zu",
                         (unsigned long)retry_count,
                         offset,
                         total_bytes);
            }
            vTaskDelay(AUDIO_PLAYER_I2S_DMA_BACKOFF_TICKS);
            continue;
        }

        ESP_LOGE(TAG, "iis_write failed: %s", esp_err_to_name(err));
        return err;
    }

    ctx->valid_samples_written += valid_samples;
    ctx->dma_samples_written += total_bytes / sizeof(int16_t);
    ctx->pcm_chunks_written++;
    s_dma_diag.total_valid_samples += valid_samples;
    s_dma_diag.total_dma_samples += total_bytes / sizeof(int16_t);
    s_dma_diag.pcm_chunk_count++;
#if ENABLE_VERBOSE_AUDIO_LOG
    ESP_LOGI(TAG,
             "speaker PCM chunk written seq=%lu valid_samples=%lu dma_samples=%lu "
             "chunk_bytes=%zu cumulative_valid_samples=%llu cumulative_dma_samples=%llu",
             (unsigned long)sequence,
             (unsigned long)valid_samples,
             (unsigned long)(total_bytes / sizeof(int16_t)),
             total_bytes,
             (unsigned long long)ctx->valid_samples_written,
             (unsigned long long)ctx->dma_samples_written);
#endif
    return ESP_OK;
}

static TickType_t speaker_player_ms_to_ticks(uint32_t timeout_ms)
{
    if (timeout_ms == 0xFFFFFFFFU) {
        return portMAX_DELAY;
    }

    TickType_t ticks = pdMS_TO_TICKS(timeout_ms);
    if (timeout_ms > 0 && ticks == 0) {
        ticks = 1;
    }
    return ticks;
}

static esp_err_t speaker_player_ring_send(audio_player_stream_ctx_t *ctx,
                                          const audio_player_ring_item_t *item)
{
    /*
     * 生产侧向环形缓冲区投递 PCM 音频块。
     * 如果写入任务已经报错退出，这里直接把错误返回给播放主流程。
     */
    if (ctx == NULL || ctx->ring_slots == NULL || ctx->ring_space == NULL ||
        ctx->ring_items == NULL || item == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (ctx->abort_requested) {
        return ESP_ERR_INVALID_STATE;
    }
    if (ctx->writer_done && ctx->result != ESP_OK) {
        return ctx->result;
    }

    if (xSemaphoreTake(ctx->ring_space,
                       speaker_player_ms_to_ticks(AUDIO_PLAYER_RING_BUFFER_SEND_TIMEOUT_MS)) != pdTRUE) {
        ctx->ring_backpressure_count++;
        ESP_LOGW(TAG,
                 "SPEAKER_RING_BACKPRESSURE action=abort_turn timeout_ms=%u waits=%lu",
                 (unsigned int)AUDIO_PLAYER_RING_BUFFER_SEND_TIMEOUT_MS,
                 (unsigned long)ctx->ring_backpressure_count);
        return ESP_ERR_TIMEOUT;
    }

    bool high_water_changed = false;
    uint32_t ring_high_water = 0;
    uint32_t ring_capacity = 0;

    portENTER_CRITICAL(&s_ring_lock);
    uint32_t slot_index = ctx->ring_write_index;
    ctx->ring_write_index = (ctx->ring_write_index + 1U) % ctx->ring_capacity;
    ctx->ring_used++;
    if (ctx->ring_used > ctx->ring_high_water) {
        ctx->ring_high_water = ctx->ring_used;
        high_water_changed = true;
        ring_high_water = ctx->ring_high_water;
        ring_capacity = ctx->ring_capacity;
    }
    portEXIT_CRITICAL(&s_ring_lock);

    if (high_water_changed) {
        ESP_LOGI(TAG, "SPEAKER_RING_WATERMARK used=%u capacity=%u",
                 (unsigned int)ring_high_water, (unsigned int)ring_capacity);
    }

    memcpy(&ctx->ring_slots[slot_index], item, sizeof(*item));
    xSemaphoreGive(ctx->ring_items);
    return ESP_OK;
}

static esp_err_t speaker_player_ring_send_end(audio_player_stream_ctx_t *ctx,
                                              uint32_t sequence)
{
    if (ctx == NULL || ctx->scratch_item == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    audio_player_ring_item_t *item = ctx->scratch_item;
    item->type = AUDIO_PLAYER_RING_ITEM_TYPE_END;
    item->generation = ctx->generation;
    item->sequence = sequence;
    item->valid_samples = 0;
    return speaker_player_ring_send(ctx, item);
}

static esp_err_t speaker_player_ring_send_start(audio_player_stream_ctx_t *ctx)
{
    if (ctx == NULL || ctx->scratch_item == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    audio_player_ring_item_t *item = ctx->scratch_item;
    item->type = AUDIO_PLAYER_RING_ITEM_TYPE_START;
    item->generation = ctx->generation;
    item->sequence = 0U;
    item->valid_samples = 0U;
    __atomic_fetch_add(&s_deferred_diag.deferred_notify_count, 1U, __ATOMIC_RELAXED);
    return speaker_player_ring_send(ctx, item);
}

static void speaker_player_reset_session_semaphores(audio_player_stream_ctx_t *ctx)
{
    if (ctx == NULL || ctx->ring_items == NULL || ctx->ring_space == NULL ||
        ctx->done == NULL || ctx->started == NULL) {
        return;
    }
    while (xSemaphoreTake(ctx->ring_items, 0) == pdTRUE) {
    }
    while (xSemaphoreTake(ctx->ring_space, 0) == pdTRUE) {
    }
    for (uint32_t i = 0; i < ctx->ring_capacity; ++i) {
        (void)xSemaphoreGive(ctx->ring_space);
    }
    while (xSemaphoreTake(ctx->done, 0) == pdTRUE) {
    }
    while (xSemaphoreTake(ctx->started, 0) == pdTRUE) {
    }
}

static void speaker_player_writer_complete_turn(audio_player_stream_ctx_t *ctx,
                                                esp_err_t result)
{
    if (ctx == NULL || ctx->writer_done) {
        return;
    }

    /* The persistent writer owns the per-turn I2S disable transition. */
    (void)iis_stop();
    ctx->result = result;
    ctx->writer_stack_high_water_bytes = app_stack_monitor_high_water();
    ctx->writer_done = true;
    xSemaphoreGive(ctx->done);
}

static void speaker_player_writer_signal_started(audio_player_stream_ctx_t *ctx,
                                                 esp_err_t result)
{
    if (ctx == NULL || ctx->writer_start_done) {
        return;
    }

    ctx->writer_start_result = result;
    ctx->writer_start_done = true;
    if (ctx->started != NULL) {
        xSemaphoreGive(ctx->started);
    }
}

static void speaker_player_iis_writer_task(void *arg)
{
    /*
     * 只有本任务会调用 iis_write()。
     * 这样播放主流程只负责投递 PCM，IIS/PDM DMA 写入集中在一个任务中完成。
     */
    audio_player_stream_ctx_t *ctx = (audio_player_stream_ctx_t *)arg;
    if (ctx == NULL || ctx->ring_slots == NULL || ctx->ring_items == NULL ||
        ctx->ring_space == NULL || ctx->done == NULL || ctx->started == NULL ||
        ctx->dma_staging == NULL) {
        /* This task owns static TCB/stack storage and must never self-delete/recreate it. */
        ESP_LOGE(TAG,
                 "TASK_EXIT task=speaker_iis_writer reason=invalid_writer_context "
                 "action=suspend_keep_static_tcb");
        while (true) {
            vTaskSuspend(NULL);
        }
    }
    app_stack_monitor_log(TAG, "speaker_iis_writer", "entry");

    while (true) {
        if (xSemaphoreTake(ctx->ring_items, pdMS_TO_TICKS(AUDIO_PLAYER_ABORT_POLL_MS)) != pdTRUE) {
            if (ctx->abort_requested && !ctx->writer_done) {
                speaker_player_writer_signal_started(ctx, ESP_ERR_INVALID_STATE);
                speaker_player_log_deferred_diag(ctx);
                speaker_player_writer_complete_turn(ctx, ESP_ERR_INVALID_STATE);
            }
            continue;
        }

        portENTER_CRITICAL(&s_ring_lock);
        uint32_t slot_index = ctx->ring_read_index;
        ctx->ring_read_index = (ctx->ring_read_index + 1U) % ctx->ring_capacity;
        if (ctx->ring_used > 0) {
            ctx->ring_used--;
        }
        portEXIT_CRITICAL(&s_ring_lock);
        audio_player_ring_item_t *item = &ctx->ring_slots[slot_index];

        bool finish_turn = false;
        esp_err_t item_result = ESP_OK;

        if (ctx->writer_done) {
            /* Abort may leave old ring entries; the next generation drops them safely. */
        } else if (item->generation != ctx->generation) {
            /* Abort may leave old ring entries; the next generation drops them safely. */
        } else if (ctx->abort_requested) {
            speaker_player_writer_signal_started(ctx, ESP_ERR_INVALID_STATE);
            finish_turn = true;
            item_result = ESP_ERR_INVALID_STATE;
        } else if (item->type == AUDIO_PLAYER_RING_ITEM_TYPE_START) {
            __atomic_fetch_add(&s_deferred_diag.writer_start_notify_count, 1U, __ATOMIC_RELAXED);
            item_result = iis_start();
            speaker_player_writer_signal_started(ctx, item_result);
            if (item_result != ESP_OK) {
                finish_turn = true;
            }
        } else if (item->type == AUDIO_PLAYER_RING_ITEM_TYPE_END) {
            __atomic_fetch_add(&s_deferred_diag.writer_end_notify_count, 1U, __ATOMIC_RELAXED);
            finish_turn = true;
            speaker_player_log_values("speaker writer end", item->sequence, 0, 0, 0);
        } else if (item->type != AUDIO_PLAYER_RING_ITEM_TYPE_PCM) {
            ESP_LOGE(TAG,
                     "speaker ringbuffer item type invalid: type=%lu seq=%lu",
                     (unsigned long)item->type,
                     (unsigned long)item->sequence);
            item_result = ESP_ERR_INVALID_ARG;
            finish_turn = true;
        } else if (item->valid_samples == 0 ||
                   item->valid_samples > AUDIO_PLAYER_PCM_CHUNK_SAMPLES) {
            ESP_LOGE(TAG,
                     "speaker PCM valid_samples invalid: seq=%lu samples=%lu",
                     (unsigned long)item->sequence,
                     (unsigned long)item->valid_samples);
            item_result = ESP_ERR_INVALID_SIZE;
            finish_turn = true;
        } else {
            memcpy(ctx->dma_staging, item->samples, AUDIO_PLAYER_PCM_CHUNK_BYTES);
            item_result = speaker_player_write_bytes_dma_streaming(ctx,
                                                                   ctx->dma_staging,
                                                                   AUDIO_PLAYER_PCM_CHUNK_BYTES,
                                                                   item->valid_samples,
                                                                   item->sequence);
            if (item_result != ESP_OK) {
                finish_turn = true;
            }
        }

        xSemaphoreGive(ctx->ring_space);

        if (finish_turn) {
            speaker_player_log_deferred_diag(ctx);
            speaker_player_writer_complete_turn(ctx, item_result);
        }
    }
}

static esp_err_t speaker_player_write_mono_pcm_to_ring_ex(audio_player_stream_ctx_t *ctx,
                                                          const int16_t *data,
                                                          uint32_t samples,
                                                          uint32_t *sequence_io,
                                                          bool send_end)
{
    /*
     * 把连续 PCM 拆成固定长度音频块。最后一包如果不足 512 个采样点，补 0 后再发送，
     * 保证写入任务每次写 IIS 的字节数一致。
     */
    uint32_t offset_samples = 0;
    uint32_t sequence = sequence_io != NULL ? *sequence_io : 0;

    while (offset_samples < samples) {
        if (ctx->writer_done && ctx->result != ESP_OK) {
            if (sequence_io != NULL) {
                *sequence_io = sequence;
            }
            return ctx->result;
        }

        uint32_t valid_samples = samples - offset_samples;
        if (valid_samples > AUDIO_PLAYER_PCM_CHUNK_SAMPLES) {
            valid_samples = AUDIO_PLAYER_PCM_CHUNK_SAMPLES;
        }

        if (ctx->scratch_item == NULL) {
            return ESP_ERR_INVALID_ARG;
        }

        audio_player_ring_item_t *item = ctx->scratch_item;
        item->type = AUDIO_PLAYER_RING_ITEM_TYPE_PCM;
        item->generation = ctx->generation;
        item->sequence = sequence;
        item->valid_samples = valid_samples;

        memcpy(item->samples,
               &data[offset_samples],
               (size_t)valid_samples * sizeof(item->samples[0]));
        if (valid_samples < AUDIO_PLAYER_PCM_CHUNK_SAMPLES) {
            memset(&item->samples[valid_samples],
                   0,
                   (size_t)(AUDIO_PLAYER_PCM_CHUNK_SAMPLES - valid_samples) *
                   sizeof(item->samples[0]));
        }

        esp_err_t send_err = speaker_player_ring_send(ctx, item);
        if (send_err != ESP_OK) {
            ESP_LOGE(TAG,
                     "send PCM chunk to speaker ringbuffer failed: %s",
                     esp_err_to_name(send_err));
            return send_err;
        }

        offset_samples += valid_samples;
        sequence++;
    }

    if (sequence_io != NULL) {
        *sequence_io = sequence;
    }
    if (send_end) {
        esp_err_t end_err = speaker_player_ring_send_end(ctx, sequence);
        if (end_err != ESP_OK) {
            ESP_LOGE(TAG,
                     "send speaker ringbuffer EOS failed: %s",
                     esp_err_to_name(end_err));
            return end_err;
        }
    }
    return ESP_OK;
}

esp_err_t audio_player_init(void)
{
    if (speaker_player_called_from_isr()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!app_runtime_guard_check_heap_integrity(TAG, "speaker_session_before_init")) {
        return ESP_FAIL;
    }
    esp_err_t err = speaker_player_ensure_mutex();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "create play mutex failed: %s", esp_err_to_name(err));
        return err;
    }
    if (s_pcm_player_state != AUDIO_PLAYER_STATE_UNINITIALIZED) {
        return ESP_OK;
    }
    if (xSemaphoreTake(s_play_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (s_pcm_player_state != AUDIO_PLAYER_STATE_UNINITIALIZED) {
        xSemaphoreGive(s_play_mutex);
        return ESP_OK;
    }

    err = iis_init();
    if (err != ESP_OK) {
        xSemaphoreGive(s_play_mutex);
        return err;
    }

    const size_t ring_item_size = sizeof(audio_player_ring_item_t);
    const size_t ring_item_count = AUDIO_PLAYER_RING_BUFFER_CHUNKS < 2U ?
                                   2U : AUDIO_PLAYER_RING_BUFFER_CHUNKS;
    s_pcm_stream_ctx.ring_slots = (audio_player_ring_item_t *)c5_mem_calloc(ring_item_count,
                                                                              ring_item_size,
                                                                              C5_MEM_PSRAM,
                                                                              "speaker_ring_slots");
    if (s_pcm_stream_ctx.ring_slots == NULL) {
        err = ESP_ERR_NO_MEM;
        goto init_fail;
    }
    s_pcm_stream_ctx.ring_capacity = (uint32_t)ring_item_count;
    if (s_pcm_stream_ctx.ring_items == NULL) {
        s_pcm_stream_ctx.ring_items = xSemaphoreCreateCountingStatic(ring_item_count,
                                                                       0,
                                                                       &s_ring_items_storage);
    }
    if (s_pcm_stream_ctx.ring_space == NULL) {
        s_pcm_stream_ctx.ring_space = xSemaphoreCreateCountingStatic(ring_item_count,
                                                                       ring_item_count,
                                                                       &s_ring_space_storage);
    }
    if (s_pcm_stream_ctx.done == NULL) {
        s_pcm_stream_ctx.done = xSemaphoreCreateBinaryStatic(&s_stream_done_storage);
    }
    if (s_pcm_stream_ctx.started == NULL) {
        s_pcm_stream_ctx.started = xSemaphoreCreateBinaryStatic(&s_stream_started_storage);
    }
    if (s_pcm_stream_ctx.ring_items == NULL || s_pcm_stream_ctx.ring_space == NULL ||
        s_pcm_stream_ctx.done == NULL || s_pcm_stream_ctx.started == NULL) {
        err = ESP_ERR_NO_MEM;
        goto init_fail;
    }
    speaker_player_reset_session_semaphores(&s_pcm_stream_ctx);
    s_pcm_stream_ctx.scratch_item =
        (audio_player_ring_item_t *)c5_mem_calloc(1,
                                                  sizeof(*s_pcm_stream_ctx.scratch_item),
                                                  C5_MEM_PSRAM,
                                                  "speaker_ring_scratch");
    if (s_pcm_stream_ctx.scratch_item == NULL) {
        err = ESP_ERR_NO_MEM;
        goto init_fail;
    }
    s_pcm_stream_ctx.dma_staging = (uint8_t *)c5_mem_alloc(AUDIO_PLAYER_PCM_CHUNK_BYTES,
                                                             C5_MEM_INTERNAL_DMA,
                                                             "speaker_i2s_dma_staging");
    if (s_pcm_stream_ctx.dma_staging == NULL) {
        err = ESP_ERR_NO_MEM;
        goto init_fail;
    }

    if (s_pcm_stream_ctx.writer_task == NULL) {
        speaker_player_log_writer_task_alloc_check();
        c5_mem_log("task_create_before_speaker_iis_writer");
        s_writer_task_stack = (StackType_t *)c5_mem_alloc(
            AUDIO_PLAYER_I2S_WRITER_STACK_WORDS * sizeof(*s_writer_task_stack),
            C5_MEM_INTERNAL_CONTROL,
            "speaker_iis_writer_stack");
        if (s_writer_task_stack == NULL) {
            err = ESP_ERR_NO_MEM;
            c5_mem_log("task_create_after_speaker_iis_writer_failed");
            goto init_fail;
        }
        s_pcm_stream_ctx.writer_task = xTaskCreateStatic(speaker_player_iis_writer_task,
                                                           "speaker_iis_writer",
                                                           AUDIO_PLAYER_I2S_WRITER_STACK_WORDS,
                                                           &s_pcm_stream_ctx,
                                                           AUDIO_PLAYER_I2S_WRITER_TASK_PRIORITY,
                                                           s_writer_task_stack,
                                                           &s_writer_task_storage);
        if (s_pcm_stream_ctx.writer_task == NULL) {
            c5_mem_free(s_writer_task_stack, "speaker_iis_writer_stack");
            s_writer_task_stack = NULL;
            err = ESP_ERR_NO_MEM;
            c5_mem_log("task_create_after_speaker_iis_writer_failed");
            goto init_fail;
        }
        ESP_LOGI(TAG,
                 "VOICE_TASK_STACK task=speaker_iis_writer bytes=%u source=internal_static",
                 (unsigned int)AUDIO_PLAYER_I2S_WRITER_TASK_STACK_SIZE);
        ESP_LOGI(TAG, "TASK_CREATE task=speaker_iis_writer");
        c5_mem_log("task_create_after_speaker_iis_writer");
    }

    i2s_chan_info_t channel_info = {};
    uint32_t dma_bytes = iis_get_info(&channel_info) == ESP_OK ?
                         channel_info.total_dma_buf_size : 0;
    s_pcm_player_state = AUDIO_PLAYER_STATE_READY_DISABLED;
    ESP_LOGI(TAG,
             "SPEAKER_PLAYER_READY internal_free=%u internal_min=%u internal_largest=%u "
             "dma_free=%u dma_largest=%u dma_total=%u ring_item_bytes=%u ring_items=%u ring_domain=psram writer_hwm=%u",
             (unsigned int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned int)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned int)heap_caps_get_free_size(MALLOC_CAP_DMA),
             (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_DMA),
             (unsigned int)dma_bytes,
             (unsigned int)ring_item_size,
             (unsigned int)ring_item_count,
             (unsigned int)speaker_player_writer_stack_high_water());
    xSemaphoreGive(s_play_mutex);
    c5_mem_log("speaker_ready");
    return ESP_OK;

init_fail:
    if (s_pcm_stream_ctx.scratch_item != NULL) {
        c5_mem_free(s_pcm_stream_ctx.scratch_item, "speaker_ring_scratch");
        s_pcm_stream_ctx.scratch_item = NULL;
    }
    if (s_pcm_stream_ctx.dma_staging != NULL) {
        c5_mem_free(s_pcm_stream_ctx.dma_staging, "speaker_i2s_dma_staging");
        s_pcm_stream_ctx.dma_staging = NULL;
    }
    if (s_pcm_stream_ctx.ring_slots != NULL) {
        c5_mem_free(s_pcm_stream_ctx.ring_slots, "speaker_ring_slots");
        s_pcm_stream_ctx.ring_slots = NULL;
    }
    (void)iis_deinit();
    xSemaphoreGive(s_play_mutex);
    return err;
}

static void speaker_player_reset_turn_locked(audio_player_stream_ctx_t *ctx)
{
    speaker_player_dma_diag_log_summary();
    speaker_player_heap_monitor_stop();
    ctx->writer_done = false;
    ctx->abort_requested = false;
    ctx->writer_start_done = false;
    ctx->result = ESP_OK;
    ctx->writer_start_result = ESP_OK;
    ctx->writer_stack_high_water_bytes = speaker_player_writer_stack_high_water();
    ctx->valid_samples_written = 0;
    ctx->dma_samples_written = 0;
    ctx->dma_bytes_written = 0;
    ctx->pcm_chunks_written = 0;
    ctx->ring_high_water = 0;
    ctx->ring_backpressure_count = 0;
    s_pcm_stream_open = false;
    s_pcm_stream_owner_task = NULL;
    s_pcm_stream_sequence = 0;
    s_pcm_player_state = AUDIO_PLAYER_STATE_READY_DISABLED;
}

/* The persistent writer is the sole I2S/DMA owner.  Resource release must wait
 * for its completion signal before freeing any buffer it can still reference. */
static esp_err_t speaker_player_stop_writer_for_release_locked(uint32_t timeout_ms)
{
    if (s_pcm_player_state == AUDIO_PLAYER_STATE_READY_DISABLED) {
        return ESP_OK;
    }
    if (s_pcm_player_state == AUDIO_PLAYER_STATE_UNINITIALIZED ||
        s_pcm_stream_ctx.writer_task == NULL || s_pcm_stream_ctx.done == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    s_pcm_stream_ctx.abort_requested = true;
    s_pcm_player_state = AUDIO_PLAYER_STATE_ABORTING;
    if (!s_pcm_stream_ctx.writer_done &&
        xSemaphoreTake(s_pcm_stream_ctx.done,
                       speaker_player_ms_to_ticks(timeout_ms)) != pdTRUE) {
        ESP_LOGE(TAG,
                 "speaker release blocked: writer did not acknowledge stop; keep I2S/DMA buffers");
        return ESP_ERR_TIMEOUT;
    }
    if (!s_pcm_stream_ctx.writer_done) {
        return ESP_ERR_INVALID_STATE;
    }

    speaker_player_reset_turn_locked(&s_pcm_stream_ctx);
    return ESP_OK;
}

esp_err_t audio_player_stream_open(void)
{
    if (speaker_player_called_from_isr()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_pcm_stream_open &&
        s_pcm_stream_owner_task == xTaskGetCurrentTaskHandle()) {
        return ESP_OK;
    }

    esp_err_t err = audio_player_init();
    if (err != ESP_OK) {
        return err;
    }
    if (xSemaphoreTake(s_play_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (s_pcm_stream_open) {
        bool owner_task = s_pcm_stream_owner_task == xTaskGetCurrentTaskHandle();
        xSemaphoreGive(s_play_mutex);
        return owner_task ? ESP_OK : ESP_ERR_INVALID_STATE;
    }
    if (s_pcm_player_state != AUDIO_PLAYER_STATE_READY_DISABLED ||
        s_pcm_stream_ctx.writer_task == NULL) {
        xSemaphoreGive(s_play_mutex);
        return ESP_ERR_INVALID_STATE;
    }

    s_pcm_player_state = AUDIO_PLAYER_STATE_OPENING;
    while (xSemaphoreTake(s_pcm_stream_ctx.done, 0) == pdTRUE) {
    }
    while (xSemaphoreTake(s_pcm_stream_ctx.started, 0) == pdTRUE) {
    }
    s_pcm_stream_ctx.writer_done = false;
    s_pcm_stream_ctx.abort_requested = false;
    s_pcm_stream_ctx.writer_start_done = false;
    s_pcm_stream_ctx.result = ESP_OK;
    s_pcm_stream_ctx.writer_start_result = ESP_OK;
    s_pcm_stream_ctx.generation++;
    if (s_pcm_stream_ctx.generation == 0) {
        s_pcm_stream_ctx.generation = 1;
    }
    s_pcm_stream_ctx.writer_stack_high_water_bytes = speaker_player_writer_stack_high_water();
    s_pcm_stream_ctx.valid_samples_written = 0;
    s_pcm_stream_ctx.dma_samples_written = 0;
    s_pcm_stream_ctx.dma_bytes_written = 0;
    s_pcm_stream_ctx.pcm_chunks_written = 0;
    s_pcm_stream_sequence = 0;
    s_deferred_diag.callback_calls = 0U;
    s_deferred_diag.deferred_notify_count = 0U;
    s_deferred_diag.illegal_context_count = 0U;
    s_deferred_diag.writer_start_notify_count = 0U;
    s_deferred_diag.writer_end_notify_count = 0U;
    s_deferred_diag.logged_generation = 0U;
    speaker_player_dma_diag_reset();
    speaker_player_heap_monitor_start();
    c5_mem_log("before_pcm_play");

    err = speaker_player_ring_send_start(&s_pcm_stream_ctx);
    if (err == ESP_OK &&
        xSemaphoreTake(s_pcm_stream_ctx.started,
                       speaker_player_ms_to_ticks(AUDIO_PLAYER_ABORT_WAIT_MS)) != pdTRUE) {
        s_pcm_stream_ctx.abort_requested = true;
        err = ESP_ERR_TIMEOUT;
    }
    if (err == ESP_OK) {
        err = s_pcm_stream_ctx.writer_start_result;
    }
    if (err != ESP_OK) {
        s_pcm_stream_ctx.abort_requested = true;
        if (!s_pcm_stream_ctx.writer_done &&
            (s_pcm_stream_ctx.done == NULL ||
             xSemaphoreTake(s_pcm_stream_ctx.done,
                            speaker_player_ms_to_ticks(AUDIO_PLAYER_ABORT_WAIT_MS)) != pdTRUE)) {
            /* Keep this generation closed until the persistent writer observes the abort. */
            s_pcm_player_state = AUDIO_PLAYER_STATE_ABORTING;
            xSemaphoreGive(s_play_mutex);
            return err;
        }
        speaker_player_reset_turn_locked(&s_pcm_stream_ctx);
        xSemaphoreGive(s_play_mutex);
        return err;
    }

    s_pcm_stream_open = true;
    s_pcm_stream_owner_task = xTaskGetCurrentTaskHandle();
    s_pcm_player_state = AUDIO_PLAYER_STATE_PLAYING;
    return ESP_OK;
}

esp_err_t audio_player_write_pcm_chunk(const int16_t *data,
                                       uint32_t samples,
                                       int sample_rate_hz)
{
    if (speaker_player_called_from_isr()) {
        return ESP_ERR_INVALID_STATE;
    }
    __atomic_fetch_add(&s_deferred_diag.callback_calls, 1U, __ATOMIC_RELAXED);
    if (!s_pcm_stream_open || s_pcm_player_state != AUDIO_PLAYER_STATE_PLAYING ||
        s_pcm_stream_ctx.ring_slots == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (samples == 0) {
        return ESP_OK;
    }
    if (data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (sample_rate_hz == (int)AUDIO_PLAYER_REQUIRED_SAMPLE_RATE_HZ) {
        return speaker_player_write_mono_pcm_to_ring_ex(&s_pcm_stream_ctx,
                                                        data,
                                                        samples,
                                                        &s_pcm_stream_sequence,
                                                        false);
    }
    ESP_LOGE(TAG,
             "unsupported PCM sample rate: got=%d supported_native=%u",
             sample_rate_hz,
             AUDIO_PLAYER_REQUIRED_SAMPLE_RATE_HZ);
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t audio_player_stream_finish(void)
{
    if (speaker_player_called_from_isr()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_pcm_stream_open) {
        return ESP_OK;
    }

    s_pcm_player_state = AUDIO_PLAYER_STATE_DRAINING;
    esp_err_t err = speaker_player_ring_send_end(&s_pcm_stream_ctx, s_pcm_stream_sequence);
    if (err == ESP_OK) {
        __atomic_fetch_add(&s_deferred_diag.deferred_notify_count, 1U, __ATOMIC_RELAXED);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "send speaker stream EOS failed: %s",
                 esp_err_to_name(err));
    }

    if (s_pcm_stream_ctx.done == NULL ||
        xSemaphoreTake(s_pcm_stream_ctx.done,
                       speaker_player_ms_to_ticks(AUDIO_PLAYER_DRAIN_TIMEOUT_MS)) != pdTRUE) {
        s_pcm_stream_ctx.abort_requested = true;
        if (s_pcm_stream_ctx.done == NULL ||
            xSemaphoreTake(s_pcm_stream_ctx.done,
                           speaker_player_ms_to_ticks(AUDIO_PLAYER_ABORT_WAIT_MS)) != pdTRUE) {
            ESP_LOGE(TAG, "speaker drain timeout; writer did not acknowledge abort");
            xSemaphoreGive(s_play_mutex);
            return ESP_ERR_TIMEOUT;
        }
        err = err == ESP_OK ? ESP_ERR_TIMEOUT : err;
    } else if (err == ESP_OK && s_pcm_stream_ctx.result != ESP_OK) {
        err = s_pcm_stream_ctx.result;
    }
#if ENABLE_VERBOSE_AUDIO_LOG
    ESP_LOGI(TAG,
             "speaker stream summary free_heap=%u min_free_heap=%u largest_free_block=%u "
             "valid_samples=%llu dma_samples=%llu dma_bytes=%llu pcm_chunks=%lu "
             "iis_writer_stack_hwm=%u caller_stack_hwm=%u",
             (unsigned int)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned int)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT),
             (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
             (unsigned long long)s_pcm_stream_ctx.valid_samples_written,
             (unsigned long long)s_pcm_stream_ctx.dma_samples_written,
             (unsigned long long)s_pcm_stream_ctx.dma_bytes_written,
             (unsigned long)s_pcm_stream_ctx.pcm_chunks_written,
             (unsigned int)s_pcm_stream_ctx.writer_stack_high_water_bytes,
             (unsigned int)speaker_player_current_stack_high_water());
#endif

    s_pcm_stream_ctx.writer_stack_high_water_bytes = speaker_player_writer_stack_high_water();
    c5_mem_log("after_pcm_play");
    speaker_player_reset_turn_locked(&s_pcm_stream_ctx);
    xSemaphoreGive(s_play_mutex);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "speaker stream playback failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "speaker stream playback complete");
    return ESP_OK;
}

esp_err_t audio_player_stream_abort(void)
{
    if (speaker_player_called_from_isr()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_pcm_stream_open) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "speaker stream abort");
    s_pcm_stream_ctx.abort_requested = true;
    s_pcm_player_state = AUDIO_PLAYER_STATE_ABORTING;

    bool owner_task = s_pcm_stream_owner_task == NULL ||
                      s_pcm_stream_owner_task == xTaskGetCurrentTaskHandle();
    if (!owner_task) {
        /*
         * 断联回调可能来自 voice_chain 任务，而流式播放资源由 server_voice_rx 打开。
         * 非 owner 只发 abort 信号；persistent writer owns IIS disable and cleanup.
         */
        return ESP_OK;
    }

    esp_err_t err = ESP_OK;
    if (s_pcm_stream_ctx.done != NULL) {
        TickType_t wait_ticks = pdMS_TO_TICKS(AUDIO_PLAYER_ABORT_WAIT_MS);
        if (wait_ticks == 0) {
            wait_ticks = 1;
        }
        if (xSemaphoreTake(s_pcm_stream_ctx.done, wait_ticks) != pdTRUE) {
            err = ESP_ERR_TIMEOUT;
        }
    }
    if (err != ESP_OK) {
        /* Keep persistent objects intact and fail closed until the writer acknowledges abort. */
        return err;
    }

    s_pcm_stream_ctx.writer_stack_high_water_bytes = speaker_player_writer_stack_high_water();
    c5_mem_log("after_pcm_play_abort");
    speaker_player_reset_turn_locked(&s_pcm_stream_ctx);
    xSemaphoreGive(s_play_mutex);
    return err;
}

esp_err_t audio_player_release_session(uint32_t timeout_ms)
{
    if (speaker_player_called_from_isr()) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = speaker_player_ensure_mutex();
    if (err != ESP_OK) {
        return err;
    }
    if (s_pcm_player_state == AUDIO_PLAYER_STATE_UNINITIALIZED) {
        return ESP_OK;
    }
    if (xSemaphoreTake(s_play_mutex, speaker_player_ms_to_ticks(timeout_ms)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    err = speaker_player_stop_writer_for_release_locked(timeout_ms);
    if (err != ESP_OK) {
        xSemaphoreGive(s_play_mutex);
        return err;
    }

    (void)app_runtime_guard_check_heap_integrity(TAG, "speaker_session_before_release");
    speaker_player_heap_monitor_stop();
    speaker_player_reset_session_semaphores(&s_pcm_stream_ctx);
    err = iis_deinit_timed(timeout_ms);
    if (err != ESP_OK) {
        xSemaphoreGive(s_play_mutex);
        return err;
    }
    if (s_pcm_stream_ctx.scratch_item != NULL) {
        c5_mem_free(s_pcm_stream_ctx.scratch_item, "speaker_ring_scratch");
        s_pcm_stream_ctx.scratch_item = NULL;
    }
    if (s_pcm_stream_ctx.dma_staging != NULL) {
        c5_mem_free(s_pcm_stream_ctx.dma_staging, "speaker_i2s_dma_staging");
        s_pcm_stream_ctx.dma_staging = NULL;
    }
    if (s_pcm_stream_ctx.ring_slots != NULL) {
        c5_mem_free(s_pcm_stream_ctx.ring_slots, "speaker_ring_slots");
        s_pcm_stream_ctx.ring_slots = NULL;
    }
    s_pcm_stream_ctx.ring_capacity = 0U;
    s_pcm_stream_ctx.ring_read_index = 0U;
    s_pcm_stream_ctx.ring_write_index = 0U;
    s_pcm_stream_ctx.ring_used = 0U;
    s_pcm_stream_ctx.writer_done = false;
    s_pcm_stream_ctx.abort_requested = false;
    s_pcm_stream_open = false;
    s_pcm_stream_owner_task = NULL;
    s_pcm_stream_sequence = 0U;
    s_pcm_player_state = AUDIO_PLAYER_STATE_UNINITIALIZED;
    c5_mem_log("after_i2s_deinit");
    xSemaphoreGive(s_play_mutex);
    return ESP_OK;
}

esp_err_t audio_player_play_pcm(const int16_t *data, uint32_t samples)
{
    if (speaker_player_called_from_isr()) {
        return ESP_ERR_INVALID_STATE;
    }
    speaker_player_log_values("audio_player_play_pcm", samples, 0, 0, 0);

    if (samples == 0) {
        return ESP_OK;
    }
    if (data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if ((size_t)samples > SIZE_MAX / sizeof(data[0])) {
        return ESP_ERR_INVALID_SIZE;
    }

    size_t pcm_bytes = (size_t)samples * sizeof(data[0]);
    speaker_player_log_values("PDM TX write format",
                              AUDIO_PLAYER_REQUIRED_SAMPLE_RATE_HZ,
                              (int64_t)pcm_bytes,
                              ((int64_t)samples * 1000LL) / AUDIO_PLAYER_REQUIRED_SAMPLE_RATE_HZ,
                              0);

    esp_err_t err = audio_player_stream_open();
    if (err == ESP_OK) {
        err = audio_player_write_pcm_chunk(data,
                                           samples,
                                           AUDIO_PLAYER_REQUIRED_SAMPLE_RATE_HZ);
    }
    if (err != ESP_OK) {
        (void)audio_player_stream_abort();
        ESP_LOGE(TAG, "PCM playback failed: %s", esp_err_to_name(err));
        return err;
    }
    return audio_player_stream_finish();
}

esp_err_t audio_player_play_16k_pcm(const int16_t *data,
                                    uint32_t samples,
                                    int sample_rate_hz)
{
    if (speaker_player_called_from_isr()) {
        return ESP_ERR_INVALID_STATE;
    }
    speaker_player_log_values("audio_player_play_16k_pcm",
                              sample_rate_hz,
                              samples,
                              AUDIO_PLAYER_REQUIRED_SAMPLE_RATE_HZ,
                              0);

    if (samples == 0) {
        return ESP_OK;
    }
    if (data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (sample_rate_hz != (int)AUDIO_PLAYER_REQUIRED_SAMPLE_RATE_HZ) {
        ESP_LOGE(TAG,
                 "unsupported PCM sample rate: got=%d supported_native=%u",
                 sample_rate_hz,
                 AUDIO_PLAYER_REQUIRED_SAMPLE_RATE_HZ);
        return ESP_ERR_NOT_SUPPORTED;
    }

    return audio_player_play_pcm(data, samples);
}

esp_err_t audio_player_self_test_1khz(uint32_t duration_ms)
{
    if (speaker_player_called_from_isr()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (duration_ms == 0) {
        duration_ms = 1000;
    }

    const uint32_t sample_rate_hz = AUDIO_PLAYER_REQUIRED_SAMPLE_RATE_HZ;
    const uint32_t frequency_hz = 1000;
    const int16_t amplitude = 12000;
    uint32_t total_samples = (uint32_t)(((uint64_t)sample_rate_hz * duration_ms) / 1000ULL);
    if (total_samples == 0) {
        total_samples = sample_rate_hz;
    }

    ESP_LOGI(TAG,
             "speaker self-test start waveform=square freq=%luHz sample_rate=%luHz bits=16 mono duration_ms=%lu samples=%lu",
             (unsigned long)frequency_hz,
             (unsigned long)sample_rate_hz,
             (unsigned long)duration_ms,
             (unsigned long)total_samples);

    esp_err_t ret = audio_player_stream_open();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "speaker self-test stream open failed: %s", esp_err_to_name(ret));
        return ret;
    }

    uint32_t generated = 0;
    int16_t *chunk = s_self_test_chunk;
    const uint32_t samples_per_period = sample_rate_hz / frequency_hz;
    const uint32_t half_period = samples_per_period / 2U;

    while (generated < total_samples) {
        uint32_t chunk_samples = total_samples - generated;
        if (chunk_samples > AUDIO_PLAYER_PCM_CHUNK_SAMPLES) {
            chunk_samples = AUDIO_PLAYER_PCM_CHUNK_SAMPLES;
        }

        for (uint32_t i = 0; i < chunk_samples; i++) {
            uint32_t phase = (generated + i) % samples_per_period;
            chunk[i] = phase < half_period ? amplitude : (int16_t)-amplitude;
        }

        ret = audio_player_write_pcm_chunk(chunk,
                                           chunk_samples,
                                           AUDIO_PLAYER_REQUIRED_SAMPLE_RATE_HZ);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG,
                     "speaker self-test write failed generated=%lu total=%lu ret=%s",
                     (unsigned long)generated,
                     (unsigned long)total_samples,
                     esp_err_to_name(ret));
            break;
        }
        generated += chunk_samples;
    }

    esp_err_t finish_ret = audio_player_stream_finish();
    if (ret == ESP_OK && finish_ret != ESP_OK) {
        ret = finish_ret;
    }
    ESP_LOGI(TAG,
             "speaker self-test done ret=%s generated_samples=%lu total_samples=%lu",
             esp_err_to_name(ret),
             (unsigned long)generated,
             (unsigned long)total_samples);
    return ret;
}
