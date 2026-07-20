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
#include "freertos/ringbuf.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "iis.h"

static const char *TAG = "speaker_player";

/* 环形缓冲区内部条目类型，仅供本实现的 writer task 识别。 */
#define AUDIO_PLAYER_RING_ITEM_TYPE_PCM 1U
#define AUDIO_PLAYER_RING_ITEM_TYPE_END 2U

/* 播放互斥锁：同一时刻只允许一个 PCM 流占用 IIS/PDM 输出。 */
static SemaphoreHandle_t s_play_mutex = NULL;

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
    uint32_t sequence;
    uint32_t valid_samples;
    int16_t samples[AUDIO_PLAYER_PCM_CHUNK_SAMPLES];
} audio_player_ring_item_t;

/* 单次播放流的上下文，只在 audio_player_play_pcm() 生命周期内有效。 */
typedef struct {
    RingbufHandle_t ringbuf;
    SemaphoreHandle_t done;
    volatile bool writer_done;
    esp_err_t result;
    UBaseType_t writer_stack_high_water_bytes;
    audio_player_ring_item_t *scratch_item;
    uint64_t valid_samples_written;
    uint64_t dma_samples_written;
    uint64_t dma_bytes_written;
    uint32_t pcm_chunks_written;
} audio_player_stream_ctx_t;

static audio_player_stream_ctx_t s_pcm_stream_ctx = {0};
static bool s_pcm_stream_open;
static uint32_t s_pcm_stream_sequence;

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
    s_play_mutex = xSemaphoreCreateMutex();
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

static esp_err_t speaker_player_ring_send(const audio_player_stream_ctx_t *ctx,
                                          const audio_player_ring_item_t *item)
{
    /*
     * 生产侧向环形缓冲区投递 PCM 音频块。
     * 如果写入任务已经报错退出，这里直接把错误返回给播放主流程。
     */
    if (ctx == NULL || ctx->ringbuf == NULL || item == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    while (true) {
        if (ctx->writer_done && ctx->result != ESP_OK) {
            return ctx->result;
        }

        BaseType_t sent = xRingbufferSend(ctx->ringbuf,
                                          item,
                                          sizeof(*item),
                                          speaker_player_ms_to_ticks(AUDIO_PLAYER_RING_BUFFER_SEND_TIMEOUT_MS));
        if (sent == pdTRUE) {
            return ESP_OK;
        }

#if APP_DEBUG_SPEAKER_PLAYER_LOG
        ESP_LOGW(TAG,
                 "speaker ringbuffer waiting: type=%lu seq=%lu samples=%lu",
                 (unsigned long)item->type,
                 (unsigned long)item->sequence,
                 (unsigned long)item->valid_samples);
#endif
    }
}

static esp_err_t speaker_player_ring_send_end(audio_player_stream_ctx_t *ctx,
                                              uint32_t sequence)
{
    if (ctx == NULL || ctx->scratch_item == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    audio_player_ring_item_t *item = ctx->scratch_item;
    item->type = AUDIO_PLAYER_RING_ITEM_TYPE_END;
    item->sequence = sequence;
    item->valid_samples = 0;
    return speaker_player_ring_send(ctx, item);
}

static void speaker_player_iis_writer_task(void *arg)
{
    /*
     * 只有本任务会调用 iis_write()。
     * 这样播放主流程只负责投递 PCM，IIS/PDM DMA 写入集中在一个任务中完成。
     */
    audio_player_stream_ctx_t *ctx = (audio_player_stream_ctx_t *)arg;
    esp_err_t result = ESP_OK;

    if (ctx == NULL || ctx->ringbuf == NULL || ctx->done == NULL) {
        vTaskDelete(NULL);
        return;
    }
    app_stack_monitor_log(TAG, "speaker_iis_writer", "entry");

    while (true) {
        size_t item_size = 0;
        audio_player_ring_item_t *item =
            (audio_player_ring_item_t *)xRingbufferReceive(ctx->ringbuf,
                                                           &item_size,
                                                           portMAX_DELAY);
        if (item == NULL) {
            result = result == ESP_OK ? ESP_ERR_TIMEOUT : result;
            break;
        }

        bool end_of_stream = false;
        esp_err_t item_result = ESP_OK;

        if (item_size != sizeof(*item)) {
            ESP_LOGE(TAG,
                     "speaker ringbuffer item size mismatch: got=%zu expected=%zu",
                     item_size,
                     sizeof(*item));
            item_result = ESP_ERR_INVALID_SIZE;
        } else if (item->type == AUDIO_PLAYER_RING_ITEM_TYPE_END) {
            end_of_stream = true;
            speaker_player_log_values("speaker writer end", item->sequence, 0, 0, 0);
        } else if (item->type != AUDIO_PLAYER_RING_ITEM_TYPE_PCM) {
            ESP_LOGE(TAG,
                     "speaker ringbuffer item type invalid: type=%lu seq=%lu",
                     (unsigned long)item->type,
                     (unsigned long)item->sequence);
            item_result = ESP_ERR_INVALID_ARG;
        } else if (item->valid_samples == 0 ||
                   item->valid_samples > AUDIO_PLAYER_PCM_CHUNK_SAMPLES) {
            ESP_LOGE(TAG,
                     "speaker PCM valid_samples invalid: seq=%lu samples=%lu",
                     (unsigned long)item->sequence,
                     (unsigned long)item->valid_samples);
            item_result = ESP_ERR_INVALID_SIZE;
        } else if (result == ESP_OK) {
            item_result = speaker_player_write_bytes_dma_streaming(ctx,
                                                                   item->samples,
                                                                   AUDIO_PLAYER_PCM_CHUNK_BYTES,
                                                                   item->valid_samples,
                                                                   item->sequence);
        }

        vRingbufferReturnItem(ctx->ringbuf, item);

        if (result == ESP_OK && item_result != ESP_OK) {
            result = item_result;
        }
        if (end_of_stream) {
            break;
        }
    }

    ctx->writer_stack_high_water_bytes = app_stack_monitor_log(TAG,
                                                               "speaker_iis_writer",
                                                               "exit");
#if ENABLE_VERBOSE_AUDIO_LOG
    ESP_LOGI(TAG,
             "speaker IIS writer task summary result=%s pcm_chunks=%lu valid_samples=%llu "
             "dma_samples=%llu dma_bytes=%llu stack_hwm=%u free_heap=%u min_free_heap=%u "
             "largest_free_block=%u",
             esp_err_to_name(result),
             (unsigned long)ctx->pcm_chunks_written,
             (unsigned long long)ctx->valid_samples_written,
             (unsigned long long)ctx->dma_samples_written,
             (unsigned long long)ctx->dma_bytes_written,
             (unsigned int)ctx->writer_stack_high_water_bytes,
             (unsigned int)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned int)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT),
             (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
#endif
    ctx->result = result;
    ctx->writer_done = true;
    xSemaphoreGive(ctx->done);
    vTaskDelete(NULL);
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

static esp_err_t speaker_player_write_mono_pcm_to_ring(audio_player_stream_ctx_t *ctx,
                                                       const int16_t *data,
                                                       uint32_t samples)
{
    uint32_t sequence = 0;
    return speaker_player_write_mono_pcm_to_ring_ex(ctx, data, samples, &sequence, true);
}

esp_err_t audio_player_init(void)
{
    /* 只初始化播放器互斥锁；IIS/PDM channel 延迟到真正播放第一块 PCM 时再创建。 */
    esp_err_t err = speaker_player_ensure_mutex();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "create play mutex failed: %s", esp_err_to_name(err));
        return err;
    }
    return ESP_OK;
}

static void speaker_player_stream_cleanup_locked(audio_player_stream_ctx_t *ctx,
                                                 bool destroy_buffers)
{
    if (ctx == NULL) {
        return;
    }

    (void)iis_stop();

    if (destroy_buffers && ctx->ringbuf != NULL) {
        vRingbufferDelete(ctx->ringbuf);
        ctx->ringbuf = NULL;
    }
    if (destroy_buffers && ctx->done != NULL) {
        vSemaphoreDelete(ctx->done);
        ctx->done = NULL;
    }
    if (destroy_buffers && ctx->scratch_item != NULL) {
        heap_caps_free(ctx->scratch_item);
        ctx->scratch_item = NULL;
    }
    ctx->writer_done = false;
    ctx->result = ESP_OK;
    speaker_player_dma_diag_log_summary();
    speaker_player_heap_monitor_stop();
    if (destroy_buffers) {
        esp_err_t deinit_ret = iis_deinit();
        if (deinit_ret != ESP_OK) {
            ESP_LOGW(TAG, "speaker IIS deinit failed: %s", esp_err_to_name(deinit_ret));
        }
#if ENABLE_VERBOSE_AUDIO_LOG
        ESP_LOGI(TAG,
                 "speaker stream cleanup done free_heap=%u min_free_heap=%u largest_free_block=%u",
                 (unsigned int)heap_caps_get_free_size(MALLOC_CAP_8BIT),
                 (unsigned int)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT),
                 (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
#endif
    }
}

esp_err_t audio_player_stream_open(void)
{
    if (s_pcm_stream_open) {
        return ESP_OK;
    }

    esp_err_t err = audio_player_init();
    if (err != ESP_OK) {
        return err;
    }
    if (xSemaphoreTake(s_play_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    s_pcm_stream_ctx.writer_done = false;
    s_pcm_stream_ctx.result = ESP_OK;
    s_pcm_stream_ctx.writer_stack_high_water_bytes = 0;
    s_pcm_stream_ctx.valid_samples_written = 0;
    s_pcm_stream_ctx.dma_samples_written = 0;
    s_pcm_stream_ctx.dma_bytes_written = 0;
    s_pcm_stream_ctx.pcm_chunks_written = 0;
    s_pcm_stream_sequence = 0;

    speaker_player_dma_diag_reset();
    speaker_player_heap_monitor_start();

    err = iis_init();
    if (err != ESP_OK) {
        goto open_fail;
    }

    i2s_chan_info_t play_chan_info = {};
    if (iis_get_info(&play_chan_info) == ESP_OK) {
        speaker_player_log_values("stream_open DMA diagnostic",
                                  IIS_EFFECTIVE_DMA_DESC_NUM,
                                  IIS_EFFECTIVE_DMA_FRAME_NUM,
                                  play_chan_info.total_dma_buf_size,
                                  0);
    }

    const size_t ring_item_size = sizeof(audio_player_ring_item_t);
    const size_t ring_item_count = AUDIO_PLAYER_RING_BUFFER_CHUNKS < 2U ?
                                   2U : AUDIO_PLAYER_RING_BUFFER_CHUNKS;

#if ENABLE_VERBOSE_AUDIO_LOG
    bool reused_ringbuffer = s_pcm_stream_ctx.ringbuf != NULL;
#endif
    if (s_pcm_stream_ctx.ringbuf == NULL) {
        s_pcm_stream_ctx.ringbuf = xRingbufferCreateNoSplit(ring_item_size, ring_item_count);
        if (s_pcm_stream_ctx.ringbuf == NULL) {
            ESP_LOGE(TAG,
                     "create speaker ringbuffer failed: item_size=%zu item_count=%zu",
                     ring_item_size,
                     ring_item_count);
            err = ESP_ERR_NO_MEM;
            goto open_fail;
        }
    }

    if (s_pcm_stream_ctx.done == NULL) {
        s_pcm_stream_ctx.done = xSemaphoreCreateBinary();
        if (s_pcm_stream_ctx.done == NULL) {
            ESP_LOGE(TAG, "create speaker writer done semaphore failed");
            err = ESP_ERR_NO_MEM;
            goto open_fail;
        }
    }

    if (s_pcm_stream_ctx.scratch_item == NULL) {
        s_pcm_stream_ctx.scratch_item =
            (audio_player_ring_item_t *)heap_caps_calloc(1,
                                                         sizeof(*s_pcm_stream_ctx.scratch_item),
                                                         MALLOC_CAP_8BIT);
        if (s_pcm_stream_ctx.scratch_item == NULL) {
            ESP_LOGE(TAG,
                     "alloc speaker stream scratch item failed bytes=%u",
                     (unsigned int)sizeof(*s_pcm_stream_ctx.scratch_item));
            err = ESP_ERR_NO_MEM;
            goto open_fail;
        }
    }

    err = iis_start();
    if (err != ESP_OK) {
        goto open_fail;
    }

    BaseType_t task_created = xTaskCreate(speaker_player_iis_writer_task,
                                          "speaker_iis_writer",
                                          AUDIO_PLAYER_I2S_WRITER_TASK_STACK_SIZE,
                                          &s_pcm_stream_ctx,
                                          AUDIO_PLAYER_I2S_WRITER_TASK_PRIORITY,
                                          NULL);
    if (task_created != pdPASS) {
        ESP_LOGE(TAG, "create speaker_iis_writer task failed");
        err = ESP_ERR_NO_MEM;
        goto open_fail;
    }

    s_pcm_stream_open = true;
#if ENABLE_VERBOSE_AUDIO_LOG
    ESP_LOGI(TAG,
             "speaker stream open ok ring_item_size=%u item_count=%u reuse_ringbuffer=%d free_heap=%u largest_free_block=%u",
             (unsigned int)ring_item_size,
             (unsigned int)ring_item_count,
             reused_ringbuffer ? 1 : 0,
             (unsigned int)esp_get_free_heap_size(),
             (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
#endif
    return ESP_OK;

open_fail:
    speaker_player_stream_cleanup_locked(&s_pcm_stream_ctx, true);
    xSemaphoreGive(s_play_mutex);
    return err;
}

esp_err_t audio_player_write_pcm_chunk(const int16_t *data,
                                       uint32_t samples,
                                       int sample_rate_hz)
{
    if (!s_pcm_stream_open || s_pcm_stream_ctx.ringbuf == NULL) {
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
    if (!s_pcm_stream_open) {
        return ESP_OK;
    }

    esp_err_t err = speaker_player_ring_send_end(&s_pcm_stream_ctx, s_pcm_stream_sequence);
    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "send speaker stream EOS failed: %s",
                 esp_err_to_name(err));
    }

    if (s_pcm_stream_ctx.done == NULL ||
        xSemaphoreTake(s_pcm_stream_ctx.done, portMAX_DELAY) != pdTRUE) {
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

    speaker_player_stream_cleanup_locked(&s_pcm_stream_ctx, true);
    s_pcm_stream_open = false;
    s_pcm_stream_sequence = 0;
    xSemaphoreGive(s_play_mutex);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "speaker stream playback failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "speaker stream playback complete");
    return ESP_OK;
}

esp_err_t audio_player_play_pcm(const int16_t *data, uint32_t samples)
{
    /*
     * 一次播放的完整生命周期：
     * 建环形缓冲区 -> 启动 IIS -> 启动写入任务 -> 投递 PCM -> 等待写入任务收尾。
     */
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

    esp_err_t err = audio_player_init();
    if (err != ESP_OK) {
        return err;
    }
    if (xSemaphoreTake(s_play_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    audio_player_stream_ctx_t stream_ctx = {
        .ringbuf = NULL,
        .done = NULL,
        .writer_done = false,
        .result = ESP_OK,
        .scratch_item = NULL,
    };

    speaker_player_dma_diag_reset();
    speaker_player_heap_monitor_start();

    err = iis_init();
    if (err != ESP_OK) {
        goto play_cleanup;
    }

    i2s_chan_info_t play_chan_info = {};
    if (iis_get_info(&play_chan_info) == ESP_OK) {
        speaker_player_log_values("play_start DMA diagnostic",
                                  IIS_EFFECTIVE_DMA_DESC_NUM,
                                  IIS_EFFECTIVE_DMA_FRAME_NUM,
                                  play_chan_info.total_dma_buf_size,
                                  0);
    }
    const size_t ring_item_size = sizeof(audio_player_ring_item_t);
    const size_t ring_item_count = AUDIO_PLAYER_RING_BUFFER_CHUNKS < 2U ?
                                   2U : AUDIO_PLAYER_RING_BUFFER_CHUNKS;

    stream_ctx.ringbuf = xRingbufferCreateNoSplit(ring_item_size, ring_item_count);
    if (stream_ctx.ringbuf == NULL) {
        ESP_LOGE(TAG,
                 "create speaker ringbuffer failed: item_size=%zu item_count=%zu",
                 ring_item_size,
                 ring_item_count);
        err = ESP_ERR_NO_MEM;
        goto play_cleanup;
    }

    stream_ctx.done = xSemaphoreCreateBinary();
    if (stream_ctx.done == NULL) {
        ESP_LOGE(TAG, "create speaker writer done semaphore failed");
        err = ESP_ERR_NO_MEM;
        goto play_cleanup;
    }

    stream_ctx.scratch_item =
        (audio_player_ring_item_t *)heap_caps_calloc(1,
                                                     sizeof(*stream_ctx.scratch_item),
                                                     MALLOC_CAP_8BIT);
    if (stream_ctx.scratch_item == NULL) {
        ESP_LOGE(TAG,
                 "alloc speaker playback scratch item failed bytes=%u",
                 (unsigned int)sizeof(*stream_ctx.scratch_item));
        err = ESP_ERR_NO_MEM;
        goto play_cleanup;
    }

    speaker_player_log_values("speaker ringbuffer ready",
                              AUDIO_PLAYER_PCM_CHUNK_SAMPLES,
                              (int64_t)ring_item_size,
                              (int64_t)ring_item_count,
                              0);

    err = iis_start();
    if (err != ESP_OK) {
        goto play_cleanup;
    }

    BaseType_t task_created = xTaskCreate(speaker_player_iis_writer_task,
                                          "speaker_iis_writer",
                                          AUDIO_PLAYER_I2S_WRITER_TASK_STACK_SIZE,
                                          &stream_ctx,
                                          AUDIO_PLAYER_I2S_WRITER_TASK_PRIORITY,
                                          NULL);
    if (task_created != pdPASS) {
        ESP_LOGE(TAG, "create speaker_iis_writer task failed");
        err = ESP_ERR_NO_MEM;
        goto play_cleanup;
    }

    err = speaker_player_write_mono_pcm_to_ring(&stream_ctx, data, samples);
    if (err != ESP_OK) {
        (void)speaker_player_ring_send_end(&stream_ctx, 0);
    }

    if (xSemaphoreTake(stream_ctx.done, portMAX_DELAY) != pdTRUE) {
        err = err == ESP_OK ? ESP_ERR_TIMEOUT : err;
    } else if (err == ESP_OK && stream_ctx.result != ESP_OK) {
        err = stream_ctx.result;
    }

play_cleanup:
    esp_err_t stop_err = iis_stop();
    if (err == ESP_OK && stop_err != ESP_OK) {
        err = stop_err;
    }
    esp_err_t deinit_err = iis_deinit();
    if (err == ESP_OK && deinit_err != ESP_OK) {
        err = deinit_err;
    }

    if (stream_ctx.ringbuf != NULL) {
        vRingbufferDelete(stream_ctx.ringbuf);
    }
    if (stream_ctx.done != NULL) {
        vSemaphoreDelete(stream_ctx.done);
    }
    if (stream_ctx.scratch_item != NULL) {
        heap_caps_free(stream_ctx.scratch_item);
    }

    speaker_player_dma_diag_log_summary();
    speaker_player_heap_monitor_stop();
    xSemaphoreGive(s_play_mutex);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "PCM playback failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG,
             "PCM playback complete input_samples=%lu valid_samples=%llu dma_samples=%llu dma_bytes=%llu",
             (unsigned long)samples,
             (unsigned long long)stream_ctx.valid_samples_written,
             (unsigned long long)stream_ctx.dma_samples_written,
             (unsigned long long)stream_ctx.dma_bytes_written);
    return ESP_OK;
}

esp_err_t audio_player_play_16k_pcm(const int16_t *data,
                                    uint32_t samples,
                                    int sample_rate_hz)
{
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
    int16_t *chunk = (int16_t *)heap_caps_malloc(AUDIO_PLAYER_PCM_CHUNK_BYTES, MALLOC_CAP_8BIT);
    if (chunk == NULL) {
        (void)audio_player_stream_finish();
        ESP_LOGE(TAG,
                 "speaker self-test chunk alloc failed bytes=%u",
                 (unsigned int)AUDIO_PLAYER_PCM_CHUNK_BYTES);
        return ESP_ERR_NO_MEM;
    }
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
    heap_caps_free(chunk);

    ESP_LOGI(TAG,
             "speaker self-test done ret=%s generated_samples=%lu total_samples=%lu",
             esp_err_to_name(ret),
             (unsigned long)generated,
             (unsigned long)total_samples);
    return ret;
}
