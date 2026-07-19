#include "mic_adc_test.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "app_stack_monitor.h"
#include "c5_memory.h"
#include "gateway_link.h"
#include "local_wake_word.h"
#include "mic_adc_pcm.h"
#include "mic_vad.h"
#include "wifi_manager.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_adc/adc_continuous.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#if MIC_ADC_SAMPLE_FREQ_HZ != MIC_ADC_PCM_SAMPLE_RATE_HZ
#error "MIC_ADC_SAMPLE_FREQ_HZ must match MIC_ADC_PCM_SAMPLE_RATE_HZ"
#endif

/* 日志标签仅供本模块内部使用，不作为后期调试参数暴露。 */
static const char *TAG = "mic_adc_test";

/* ADC continuous 句柄：由 mic_adc_test_start() 初始化，采样任务只借用该句柄读取数据。 */
static adc_continuous_handle_t s_adc_handle;

/* 任务句柄：用于防止重复创建 Mic ADC 测试任务。 */
static TaskHandle_t s_mic_adc_task_handle;
#define MIC_ADC_TEST_TASK_STACK_WORDS \
    ((MIC_ADC_TEST_TASK_STACK_SIZE + sizeof(StackType_t) - 1U) / sizeof(StackType_t))
static StackType_t *s_mic_adc_task_stack;
static StaticTask_t s_mic_adc_task_storage;
/* Only the Mic task may transition the ADC continuous driver. */
static TaskHandle_t s_mic_adc_owner_task;
static EventGroupHandle_t s_mic_adc_control_events;
static volatile bool s_mic_adc_started;
static volatile uint32_t s_mic_session_generation;
static mic_adc_voice_stream_ops_t s_mic_adc_voice_stream_ops;
static bool s_mic_adc_voice_stream_ops_registered;

enum {
    MIC_ADC_CONTROL_PAUSE_REQUEST_BIT = BIT0,
    MIC_ADC_CONTROL_PAUSED_BIT = BIT1,
    MIC_ADC_CONTROL_STOP_REQUEST_BIT = BIT2,
    MIC_ADC_CONTROL_STOPPED_BIT = BIT3,
    MIC_CTRL_INIT_READY = BIT4,
    MIC_CTRL_INIT_ABORT = BIT5,
    MIC_ADC_CONTROL_RUNNING_BIT = BIT6,
};

/* The pre-roll ring is 16 KiB at the current settings, so keep it out of internal RAM. */
static int16_t *s_mic_voice_pre_roll_storage;
static int16_t s_mic_voice_live_chunk_storage[MIC_ADC_VOICE_LIVE_CHUNK_SAMPLES];
static uint8_t s_mic_adc_raw_buffer[MIC_ADC_READ_BYTES];
static adc_continuous_data_t s_mic_adc_parsed_buffer[MIC_ADC_READ_BYTES / SOC_ADC_DIGI_RESULT_BYTES];

/**
 * @brief mic_adc_test 内部的 voice stream 会话状态。
 *
 * 状态含义：
 * - IDLE：未建立 server voice turn，只允许把当前安静期 PCM 写入 pre-roll 环形缓存。
 * - STREAMING：VOICE_START 后 server voice turn 成功，只允许在这个状态发送 pre-roll 和实时 PCM。
 * - POST_ROLL：外层 VAD VOICE_END 后继续发送短尾音，避免句尾被本地 VAD 截断。
 * - FINISHING：post-roll 完成后进入，正在调用 finish/stop 收尾，禁止继续发送 pre-roll 和 PCM。
 */
typedef enum {
    MIC_ADC_VOICE_STATE_IDLE = 0,    // 空闲态：只维护下一句话的句首预缓存。
    MIC_ADC_VOICE_STATE_STREAMING,   // 流式态：当前 server voice turn 可接收 pre-roll 和 PCM。
    MIC_ADC_VOICE_STATE_POST_ROLL,   // 尾音态：VOICE_END 后继续发送一小段 PCM。
    MIC_ADC_VOICE_STATE_FINISHING,   // 收尾态：当前会话正在结束，新的音频样本全部丢弃。
} mic_adc_voice_state_t;

typedef enum {
    MIC_ADC_VOICE_ACTIVATE_NONE = 0,
    MIC_ADC_VOICE_ACTIVATE_DEFERRED,
    MIC_ADC_VOICE_ACTIVATE_STREAMING,
} mic_adc_voice_activate_result_t;

/**
 * @brief Voice pre-roll and streaming state.
 *
 * 调用方法：mic_adc_test_task() 创建后先调用 mic_adc_voice_stream_init()。
 * 每个 PCM 样本调用 mic_adc_voice_stream_push_sample()，VAD 事件由
 * mic_adc_window_report() 调用 start/finish 挂接。
 */
typedef struct {
    int16_t *pre_roll_samples;       // 句首预缓存环形数组，由调用方提供。
    size_t pre_roll_capacity_samples;// 预缓存最大样本数。
    size_t pre_roll_sample_count;    // 当前有效预缓存样本数。
    size_t pre_roll_write_index;     // 下一次写入位置。
    int16_t *live_chunk_samples;      // Voice turn 已启动后的实时小批量发送缓冲，由静态存储提供。
    size_t live_chunk_capacity_samples;// live_chunk_samples 可容纳的样本数。
    size_t live_chunk_sample_count;   // 当前 live_chunk 中已累计的样本数。
    size_t post_roll_remaining_samples;// VOICE_END 后仍需继续发送的尾部 PCM 样本数。
    mic_adc_voice_state_t state;        // Voice turn 四态；STREAMING/POST_ROLL 允许上传 PCM。
    TickType_t next_start_tick;        // Voice turn 启动失败后的退避截止 tick，避免断网时频繁重连。
    TickType_t session_start_tick;     // 本轮 voice turn 打开 PCM 闸门的 tick，用于统计时长。
    TickType_t last_done_tick;         // 上一轮 voice turn done 的 tick，用于 VAD 起始冷却。
    TickType_t last_cooldown_log_tick; // 冷却忽略日志节流 tick。
    uint32_t pcm_bytes_total;          // 本轮已成功上传的 PCM 字节数。
    bool waiting_log_printed;          // 控制 voice waiting 日志只在进入等待态时打印一次。
    bool finish_busy_log_printed;      // FINISHING 期间只打印一次 busy，避免 VAD 反复触发刷屏。
    uint32_t generation;               // 当前 Mic/VAD 周期，旧回调不得复用本地 PCM 状态。
} mic_adc_voice_stream_t;

typedef struct {
    int16_t *samples;          // WakeNet 单次 detect 需要的输入缓冲。
    size_t capacity_samples;   // samples 可容纳的样本数，来自 local_wake_word_get_chunk_samples()。
    size_t sample_count;       // 当前已累计样本数。
    bool enabled;              // WakeNet 模型和输入缓冲均就绪时为 true。
    bool unavailable_logged;    // 控制不可用日志只打印一次。
} mic_adc_local_wake_t;

/* READY gate 打开前由启动线程完成这些对象的初始化，采样任务只在 gate 后借用。 */
static mic_vad_t s_mic_adc_vad;
static mic_adc_voice_stream_t s_mic_adc_voice_stream;
static mic_adc_local_wake_t s_mic_adc_local_wake;

/**
 * @brief 一个串口统计窗口内的 Mic ADC 原始数据累加状态。
 *
 * 调用方法：mic_adc_test_task() 创建局部变量，随后通过
 * mic_adc_window_reset()、mic_adc_window_add()、mic_adc_window_report() 维护。
 */
typedef struct {
    uint32_t count;          // 当前窗口已经累计的有效 ADC 样本数。
    uint32_t min_raw;        // 当前窗口的最小 raw 值，用于观察安静/说话时的摆幅下沿。
    uint32_t max_raw;        // 当前窗口的最大 raw 值，用于观察安静/说话时的摆幅上沿。
    uint32_t last_raw;       // 最近一个 raw 值，用于确认 ADC 数据正在刷新。
    uint64_t sum_raw;        // raw 值累加和，用于计算平均值，即前端直流偏置附近的中心点。
    uint64_t sum_square_raw; // raw 平方累加和，用于计算去直流后的 RMS。
    uint32_t adc_clip_low;   // ADC raw 等于 0 的次数，用于判断低端削顶。
    uint32_t adc_clip_high;  // ADC raw 等于最大值的次数，用于判断高端削顶。
    int16_t min_pcm;         // 当前窗口 PCM 最小值，用于观察转换后的负向摆幅。
    int16_t max_pcm;         // 当前窗口 PCM 最大值，用于观察转换后的正向摆幅。
    int16_t last_pcm;        // 最近一个 PCM 样本，用于确认 ADC -> PCM 链路正在刷新。
    int64_t sum_pcm;         // PCM 样本累加和，用于观察去直流后是否接近 0。
    uint64_t sum_square_pcm; // PCM 平方累加和，用于计算转换后的音频 RMS。
    uint32_t pcm_clip_low;   // PCM 等于 INT16_MIN 的次数，用于判断负向削顶。
    uint32_t pcm_clip_high;  // PCM 等于 INT16_MAX 的次数，用于判断正向削顶。
} mic_adc_window_t;

static void mic_adc_window_reset(mic_adc_window_t *window);

static const char *mic_adc_task_name(void)
{
    TaskHandle_t task = xTaskGetCurrentTaskHandle();
    const char *name = task != NULL ? pcTaskGetName(task) : NULL;
    return name != NULL ? name : "<none>";
}

static bool mic_adc_current_task_is_owner(const char *operation)
{
    TaskHandle_t current = xTaskGetCurrentTaskHandle();
    if (s_mic_adc_owner_task == current && current != NULL) {
        return true;
    }
    ESP_LOGE(TAG,
             "adc_owner_violation operation=%s adc_owner_task=%s current_task=%s",
             operation != NULL ? operation : "-",
             s_mic_adc_owner_task != NULL ? pcTaskGetName(s_mic_adc_owner_task) : "<none>",
             current != NULL ? pcTaskGetName(current) : "<none>");
    return false;
}

static uint32_t mic_adc_next_generation(void)
{
    ++s_mic_session_generation;
    if (s_mic_session_generation == 0U) {
        s_mic_session_generation = 1U;
    }
    return s_mic_session_generation;
}

static void mic_adc_log_heap(const char *label)
{
#if ENABLE_VERBOSE_AUDIO_LOG
    ESP_LOGI(TAG,
             "%s task=%s free_heap=%u min_free_heap=%u largest_free_block=%u",
             label != NULL ? label : "Mic ADC heap",
             mic_adc_task_name(),
             (unsigned int)esp_get_free_heap_size(),
             (unsigned int)esp_get_minimum_free_heap_size(),
             (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
#else
    (void)label;
#endif
}

static void mic_adc_log_start_stage(const char *stage, const char *edge, esp_err_t ret)
{
    const bool failed = ret != ESP_OK && ret != ESP_ERR_NOT_FINISHED;
    ESP_LOG_LEVEL_LOCAL(failed ? ESP_LOG_ERROR : ESP_LOG_INFO,
                        TAG,
                        "VOICE_START_STAGE stage=%s edge=%s ret=0x%x(%s) "
                        "internal_free=%u internal_min=%u internal_largest=%u "
                        "dma_free=%u dma_largest=%u psram_free=%u",
                        stage != NULL ? stage : "<none>",
                        edge != NULL ? edge : "<none>",
                        (unsigned int)ret,
                        esp_err_to_name(ret),
                        (unsigned int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                        (unsigned int)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                        (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                        (unsigned int)heap_caps_get_free_size(MALLOC_CAP_DMA),
                        (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_DMA),
                        (unsigned int)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
}

static void mic_adc_log_start_failure(const char *stage, esp_err_t ret)
{
    ESP_LOGE(TAG,
             "VOICE_START_FAIL stage=%s ret=0x%x(%s)",
             stage != NULL ? stage : "<none>",
             (unsigned int)ret,
             esp_err_to_name(ret));
    mic_adc_log_start_stage(stage, "failure", ret);
}

static bool mic_adc_pause_requested(void);
static bool mic_adc_stop_requested(void);
#if ENABLE_VERBOSE_AUDIO_LOG
static const char *mic_adc_voice_state_name(mic_adc_voice_state_t state);
static bool mic_adc_voice_backend_is_ready(void);
#endif
static bool mic_adc_voice_backend_is_idle(void);
static esp_err_t mic_adc_voice_backend_append_pcm(const int16_t *pcm, size_t samples);
static esp_err_t mic_adc_voice_backend_finish(void);
static mic_adc_voice_activate_result_t mic_adc_voice_stream_activate(mic_adc_voice_stream_t *stream);

#if ENABLE_VERBOSE_AUDIO_LOG
static const char *mic_adc_voice_state_name(mic_adc_voice_state_t state)
{
    switch (state) {
    case MIC_ADC_VOICE_STATE_IDLE:
        return "IDLE";
    case MIC_ADC_VOICE_STATE_STREAMING:
        return "STREAMING";
    case MIC_ADC_VOICE_STATE_POST_ROLL:
        return "POST_ROLL";
    case MIC_ADC_VOICE_STATE_FINISHING:
        return "FINISHING";
    default:
        return "UNKNOWN";
    }
}
#endif

static const char *mic_adc_voice_stream_name(void)
{
    if (s_mic_adc_voice_stream_ops_registered &&
        s_mic_adc_voice_stream_ops.stream_name != NULL &&
        s_mic_adc_voice_stream_ops.stream_name[0] != '\0') {
        return s_mic_adc_voice_stream_ops.stream_name;
    }
    return "server_voice_unregistered";
}

static void mic_adc_log_state(const char *label,
                              bool adc_started,
                              const mic_adc_voice_stream_t *voice_stream)
{
#if ENABLE_VERBOSE_AUDIO_LOG
    ESP_LOGI(TAG,
             "%s task=%s adc_started=%d pause_request=%d stop_request=%d streaming=%d stream=%s stream_ready=%d state=%s free_heap=%u min_free_heap=%u largest_free_block=%u",
             label != NULL ? label : "Mic ADC state",
             mic_adc_task_name(),
             adc_started ? 1 : 0,
             mic_adc_pause_requested() ? 1 : 0,
             mic_adc_stop_requested() ? 1 : 0,
             voice_stream != NULL &&
                 (voice_stream->state == MIC_ADC_VOICE_STATE_STREAMING ||
                  voice_stream->state == MIC_ADC_VOICE_STATE_POST_ROLL) ? 1 : 0,
             mic_adc_voice_stream_name(),
             mic_adc_voice_backend_is_ready() ? 1 : 0,
             voice_stream != NULL ? mic_adc_voice_state_name(voice_stream->state) : "<none>",
             (unsigned int)esp_get_free_heap_size(),
             (unsigned int)esp_get_minimum_free_heap_size(),
             (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
#else
    (void)label;
    (void)adc_started;
    (void)voice_stream;
#endif
}

/**
 * @brief 对 64 位无符号整数做整数平方根。
 *
 * 调用方法：mic_adc_window_report() 计算 RMS 时调用，避免引入浮点 sqrt 依赖。
 *
 * @param value 要开平方的无符号整数。
 * @return floor(sqrt(value))。
 */
static uint32_t mic_adc_isqrt_u64(uint64_t value)
{
    uint64_t result = 0;
    uint64_t bit = (uint64_t)1 << 62;

    while (bit > value) {
        bit >>= 2;
    }

    while (bit != 0) {
        if (value >= result + bit) {
            value -= result + bit;
            result = (result >> 1) + bit;
        } else {
            result >>= 1;
        }
        bit >>= 2;
    }

    return (uint32_t)result;
}

/**
 * @brief 初始化 server voice 流式状态。
 *
 * 调用方法：Mic ADC 任务启动时调用一次。预缓存默认 500 ms，最大由编译期检查
 * 限制为不超过 1000 ms，用于弥补 VAD 帧级判断导致的句首延迟。
 *
 * @param stream server voice 流式状态，不能为空。
 * @param pre_roll_storage 预缓存数组，不能为空。
 * @param pre_roll_capacity_samples 预缓存数组可容纳的样本数。
 */
static void mic_adc_voice_stream_init(mic_adc_voice_stream_t *stream,
                                    int16_t *pre_roll_storage,
                                    size_t pre_roll_capacity_samples,
                                    int16_t *live_chunk_storage,
                                    size_t live_chunk_capacity_samples)
{
    if (stream == NULL) {
        return;
    }

    stream->pre_roll_samples = pre_roll_storage;
    stream->pre_roll_capacity_samples = pre_roll_capacity_samples;
    stream->pre_roll_sample_count = 0;
    stream->pre_roll_write_index = 0;
    stream->live_chunk_samples = live_chunk_storage;
    stream->live_chunk_capacity_samples = live_chunk_capacity_samples;
    stream->live_chunk_sample_count = 0;
    stream->post_roll_remaining_samples = 0;
    stream->state = MIC_ADC_VOICE_STATE_IDLE;
    stream->next_start_tick = 0;
    stream->session_start_tick = 0;
    stream->last_done_tick = 0;
    stream->last_cooldown_log_tick = 0;
    stream->pcm_bytes_total = 0;
    stream->waiting_log_printed = true;
    stream->finish_busy_log_printed = false;
    stream->generation = s_mic_session_generation;
    if (APP_DEBUG_VOICE_SESSION_LOG) {
        ESP_LOGI(TAG, "voice stream waiting for speech");
    }
}

/**
 * @brief 写入一个样本到 server voice 句首预缓存。
 *
 * 调用方法：voice stream 状态为 IDLE 时，mic_adc_voice_stream_push_sample() 每收到一个 PCM 样本调用。
 * STREAMING、POST_ROLL 和 FINISHING 状态禁止写 pre-roll，避免当前会话音频或收尾期音频污染下一句话。
 *
 * @param stream server voice 流式状态，不能为空。
 * @param pcm_sample int16_t PCM 样本。
 */
static void mic_adc_voice_stream_push_pre_roll(mic_adc_voice_stream_t *stream, int16_t pcm_sample)
{
    if (stream == NULL ||
        stream->pre_roll_samples == NULL ||
        stream->pre_roll_capacity_samples == 0) {
        return;
    }

    stream->pre_roll_samples[stream->pre_roll_write_index] = pcm_sample;
    stream->pre_roll_write_index++;
    if (stream->pre_roll_write_index >= stream->pre_roll_capacity_samples) {
        stream->pre_roll_write_index = 0;
    }
    if (stream->pre_roll_sample_count < stream->pre_roll_capacity_samples) {
        stream->pre_roll_sample_count++;
    }
}

/**
 * @brief 清空 server voice 句首预缓存。
 *
 * 调用方法：pre-roll 已经发送给 server voice、voice turn start 失败或 finish 完成后调用，
 * 避免下一段语音开始太快时混入上一段缓存。
 *
 * @param stream server voice 流式状态，不能为空。
 */
static void mic_adc_voice_stream_clear_pre_roll(mic_adc_voice_stream_t *stream)
{
    if (stream == NULL) {
        return;
    }

    stream->pre_roll_sample_count = 0;
    stream->pre_roll_write_index = 0;
}

static void mic_adc_local_wake_reset(mic_adc_local_wake_t *wake)
{
    if (wake != NULL) {
        wake->sample_count = 0;
    }
    local_wake_word_reset_detector();
}

static esp_err_t mic_adc_local_wake_init(mic_adc_local_wake_t *wake)
{
    if (wake == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(wake, 0, sizeof(*wake));
    mic_adc_log_start_stage("local_wake_recording_start", "before", ESP_ERR_NOT_FINISHED);
    size_t chunk_samples = local_wake_word_get_chunk_samples();
    if (!local_wake_word_is_detection_ready() || chunk_samples == 0) {
        mic_adc_log_start_stage("local_wake_recording_start", "after", ESP_OK);
        ESP_LOGW(TAG,
                 "local WakeNet detector unavailable; standby will ignore ordinary VAD wake attempts");
        wake->unavailable_logged = true;
        return ESP_OK;
    }

    wake->samples = (int16_t *)c5_mem_alloc(chunk_samples * sizeof(int16_t),
                                             C5_MEM_INTERNAL_CONTROL,
                                             "local_wake_realtime_samples");
    if (wake->samples == NULL) {
        mic_adc_log_start_stage("local_wake_recording_start", "after", ESP_ERR_NO_MEM);
        mic_adc_log_start_failure("c5_mem_alloc(local_wake_samples)", ESP_ERR_NO_MEM);
        ESP_LOGE(TAG,
                 "local WakeNet input buffer alloc failed samples=%u",
                 (unsigned int)chunk_samples);
        return ESP_ERR_NO_MEM;
    }

    wake->capacity_samples = chunk_samples;
    wake->sample_count = 0;
    wake->enabled = true;
    mic_adc_log_start_stage("local_wake_recording_start", "after", ESP_OK);
    ESP_LOGI(TAG,
             "local WakeNet detector feed ready chunk_samples=%u bytes=%u",
             (unsigned int)chunk_samples,
             (unsigned int)(chunk_samples * sizeof(int16_t)));
    return ESP_OK;
}

static void mic_adc_local_wake_deinit(mic_adc_local_wake_t *wake)
{
    if (wake == NULL) {
        return;
    }
    if (wake->samples != NULL) {
        c5_mem_free(wake->samples, "local_wake_realtime_samples");
    }
    memset(wake, 0, sizeof(*wake));
}

static bool mic_adc_local_wake_push_idle_sample(mic_adc_local_wake_t *wake,
                                                mic_adc_voice_stream_t *voice_stream,
                                                mic_vad_t *vad,
                                                int16_t pcm_sample)
{
    if (wake == NULL || voice_stream == NULL || vad == NULL) {
        return false;
    }
    if (voice_stream->state != MIC_ADC_VOICE_STATE_IDLE ||
        local_wake_word_is_recording_window_open()) {
        wake->sample_count = 0;
        return false;
    }
    if (!wake->enabled || wake->samples == NULL || wake->capacity_samples == 0) {
        if (!wake->unavailable_logged) {
            ESP_LOGW(TAG,
                     "local WakeNet detector unavailable; ordinary VAD will remain ignored before wake");
            wake->unavailable_logged = true;
        }
        return false;
    }
    if (!local_wake_word_is_detection_ready()) {
        wake->sample_count = 0;
        return false;
    }

    wake->samples[wake->sample_count] = pcm_sample;
    wake->sample_count++;
    if (wake->sample_count < wake->capacity_samples) {
        return false;
    }

    bool detected = false;
    esp_err_t ret = local_wake_word_detect_chunk(wake->samples,
                                                 wake->capacity_samples,
                                                 &detected);
    wake->sample_count = 0;
    if (ret != ESP_OK) {
        if (ret != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "WakeNet detect chunk failed: %s", esp_err_to_name(ret));
        }
        return false;
    }
    if (!detected) {
        return false;
    }

    mic_adc_voice_stream_clear_pre_roll(voice_stream);
    mic_vad_init(vad);
    ESP_LOGI(TAG, "fixed wake word detected, request local prompt and server voice prepare");
    mic_adc_voice_activate_result_t activate_ret = mic_adc_voice_stream_activate(voice_stream);
    if (activate_ret == MIC_ADC_VOICE_ACTIVATE_NONE) {
        ESP_LOGW(TAG, "fixed wake word detected but voice chain prepare failed");
        local_wake_word_reset_detector();
    }
    return true;
}

/**
 * @brief voice turn 启动失败后的退避。
 *
 * 调用方法：WiFi 未稳定、server voice turn 打开失败或发送 pre-roll 失败后调用。这里只记录
 * 下一次允许打开 PCM 发送闸门的 tick，不阻塞 ADC 采样任务；后续 VOICE_START
 * 到来时再判断是否已经过了 MIC_ADC_VOICE_RETRY_DELAY_MS，避免断网或 server voice
 * 未 ready 时疯狂重试。
 *
 * @param stream server voice 流式状态，不能为空。
 */
static void mic_adc_voice_stream_delay_next_start(mic_adc_voice_stream_t *stream)
{
    if (stream == NULL) {
        return;
    }

    stream->next_start_tick = xTaskGetTickCount() + pdMS_TO_TICKS(MIC_ADC_VOICE_RETRY_DELAY_MS);
}

static uint32_t mic_adc_ticks_to_ms(TickType_t ticks)
{
    return (uint32_t)(((uint64_t)ticks * 1000U) / (uint64_t)configTICK_RATE_HZ);
}

static uint32_t mic_adc_voice_stream_cooldown_elapsed_ms(const mic_adc_voice_stream_t *stream,
                                                       TickType_t now)
{
    if (stream == NULL || stream->last_done_tick == 0) {
        return 0;
    }
    return mic_adc_ticks_to_ms(now - stream->last_done_tick);
}

static bool mic_adc_voice_stream_start_in_cooldown(const mic_adc_voice_stream_t *stream,
                                                 TickType_t now,
                                                 uint32_t *elapsed_ms)
{
    uint32_t elapsed = mic_adc_voice_stream_cooldown_elapsed_ms(stream, now);
    if (elapsed_ms != NULL) {
        *elapsed_ms = elapsed;
    }
    return stream != NULL &&
           stream->last_done_tick != 0 &&
           elapsed < MIC_ADC_VOICE_START_COOLDOWN_MS;
}

/**
 * @brief 判断当前是否允许启动新一轮 voice turn。
 *
 * 调用方法：mic_adc_voice_stream_activate() 在真正开始上传 PCM 前调用。
 * 返回 false 表示仍处于失败退避期，本次 VOICE_START 不启动 server voice turn。
 *
 * @param stream voice stream 状态，不能为空。
 * @return 允许启动返回 true；仍需等待返回 false。
 */
static bool mic_adc_voice_stream_can_start(mic_adc_voice_stream_t *stream)
{
    if (stream == NULL || stream->next_start_tick == 0) {
        return true;
    }

    TickType_t now = xTaskGetTickCount();
    if ((int32_t)(now - stream->next_start_tick) < 0) {
        return false;
    }

    stream->next_start_tick = 0;
    return true;
}

static esp_err_t mic_adc_prepare_voice_exclusive(void)
{
    if (s_mic_adc_voice_stream_ops_registered &&
        s_mic_adc_voice_stream_ops.prepare_cb != NULL) {
        return s_mic_adc_voice_stream_ops.prepare_cb(s_mic_adc_voice_stream_ops.user_ctx);
    }

    ESP_LOGW(TAG, "server voice prepare callback is not registered, keep Mic in VAD-only standby");
    return ESP_ERR_INVALID_STATE;
}

#if ENABLE_VERBOSE_AUDIO_LOG
static bool mic_adc_voice_backend_is_ready(void)
{
    if (s_mic_adc_voice_stream_ops_registered &&
        s_mic_adc_voice_stream_ops.is_ready_cb != NULL) {
        return s_mic_adc_voice_stream_ops.is_ready_cb(s_mic_adc_voice_stream_ops.user_ctx);
    }
    return false;
}
#endif

static bool mic_adc_voice_backend_is_idle(void)
{
    if (s_mic_adc_voice_stream_ops_registered &&
        s_mic_adc_voice_stream_ops.is_idle_cb != NULL) {
        return s_mic_adc_voice_stream_ops.is_idle_cb(s_mic_adc_voice_stream_ops.user_ctx);
    }
    return true;
}

static esp_err_t mic_adc_voice_backend_append_pcm(const int16_t *pcm, size_t samples)
{
    if (s_mic_adc_voice_stream_ops_registered &&
        s_mic_adc_voice_stream_ops.append_pcm_cb != NULL) {
        return s_mic_adc_voice_stream_ops.append_pcm_cb(pcm,
                                                        samples,
                                                        s_mic_adc_voice_stream_ops.user_ctx);
    }
    return ESP_ERR_INVALID_STATE;
}

static esp_err_t mic_adc_voice_backend_finish(void)
{
    if (s_mic_adc_voice_stream_ops_registered &&
        s_mic_adc_voice_stream_ops.finish_cb != NULL) {
        return s_mic_adc_voice_stream_ops.finish_cb(s_mic_adc_voice_stream_ops.user_ctx);
    }
    return ESP_ERR_INVALID_STATE;
}

/**
 * @brief 结束本轮 voice turn 后回到等待下一次说话。
 *
 * 调用方法：finish 成功/失败、发送失败或 pre-roll 失败后调用。函数只重置 Mic
 * 本地流式状态和小缓存；server voice 后端由 voice_chain 负责启动和收尾。
 *
 * @param stream voice stream 状态，不能为空。
 * @param log_session_done true 时打印 session done 日志。
 */
static void mic_adc_voice_stream_enter_waiting(mic_adc_voice_stream_t *stream, bool log_session_done)
{
    if (stream == NULL) {
        return;
    }

    TickType_t now = xTaskGetTickCount();
    if (log_session_done) {
        uint32_t duration_ms = stream->session_start_tick != 0 ?
                               mic_adc_ticks_to_ms(now - stream->session_start_tick) : 0;
        ESP_LOGI(TAG,
                 "voice_upload_done duration_ms=%u pcm_bytes_total=%u",
                 (unsigned int)duration_ms,
                 (unsigned int)stream->pcm_bytes_total);
        stream->last_done_tick = now;
    }

    stream->state = MIC_ADC_VOICE_STATE_IDLE;
    stream->live_chunk_sample_count = 0;
    stream->post_roll_remaining_samples = 0;
    stream->session_start_tick = 0;
    stream->pcm_bytes_total = 0;
    stream->finish_busy_log_printed = false;
    mic_adc_voice_stream_clear_pre_roll(stream);
    if (log_session_done) {
        ESP_LOGI(TAG, "Ready for next speech");
    } else if (!stream->waiting_log_printed) {
        if (APP_DEBUG_VOICE_SESSION_LOG) {
            ESP_LOGI(TAG, "voice stream waiting for speech");
        }
    }
    stream->waiting_log_printed = true;
}

static void mic_adc_reset_runtime_state(mic_adc_window_t *window,
                                        mic_adc_pcm_converter_t *pcm_converter,
                                        mic_vad_t *vad,
                                        mic_adc_voice_stream_t *voice_stream,
                                        mic_adc_local_wake_t *local_wake)
{
    const uint32_t generation = mic_adc_next_generation();
    if (pcm_converter != NULL) {
        mic_adc_pcm_converter_init(pcm_converter);
    }
    if (vad != NULL) {
        mic_vad_init(vad);
    }
    if (voice_stream != NULL) {
        mic_adc_voice_stream_enter_waiting(voice_stream, false);
        voice_stream->generation = generation;
    }
    if (local_wake != NULL) {
        mic_adc_local_wake_reset(local_wake);
    }
    if (window != NULL) {
        mic_adc_window_reset(window);
    }
    ESP_LOGI(TAG,
             "mic_generation=%lu adc_owner_task=%s",
             (unsigned long)generation,
             s_mic_adc_owner_task != NULL ? pcTaskGetName(s_mic_adc_owner_task) : "<none>");
}

static bool mic_adc_pause_requested(void)
{
    if (s_mic_adc_control_events == NULL) {
        return false;
    }
    EventBits_t bits = xEventGroupGetBits(s_mic_adc_control_events);
    return (bits & MIC_ADC_CONTROL_PAUSE_REQUEST_BIT) != 0;
}

static bool mic_adc_stop_requested(void)
{
    if (s_mic_adc_control_events == NULL) {
        return false;
    }
    EventBits_t bits = xEventGroupGetBits(s_mic_adc_control_events);
    return (bits & MIC_ADC_CONTROL_STOP_REQUEST_BIT) != 0;
}

static void mic_adc_clear_static_audio_storage(void)
{
    if (s_mic_voice_pre_roll_storage != NULL) {
        memset(s_mic_voice_pre_roll_storage,
               0,
               MIC_ADC_VOICE_PRE_ROLL_SAMPLES * sizeof(*s_mic_voice_pre_roll_storage));
    }
    memset(s_mic_voice_live_chunk_storage, 0, sizeof(s_mic_voice_live_chunk_storage));
    memset(s_mic_adc_raw_buffer, 0, sizeof(s_mic_adc_raw_buffer));
    memset(s_mic_adc_parsed_buffer, 0, sizeof(s_mic_adc_parsed_buffer));
}

static esp_err_t mic_adc_allocate_psram_audio_storage(void)
{
    if (s_mic_voice_pre_roll_storage != NULL) {
        return ESP_OK;
    }

    s_mic_voice_pre_roll_storage = heap_caps_malloc(
        MIC_ADC_VOICE_PRE_ROLL_SAMPLES * sizeof(*s_mic_voice_pre_roll_storage),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_mic_voice_pre_roll_storage == NULL) {
        return ESP_ERR_NO_MEM;
    }
    memset(s_mic_voice_pre_roll_storage,
           0,
           MIC_ADC_VOICE_PRE_ROLL_SAMPLES * sizeof(*s_mic_voice_pre_roll_storage));
    ESP_LOGI(TAG,
             "VOICE_BUFFER owner=mic_pre_roll bytes=%u source=psram",
             (unsigned int)(MIC_ADC_VOICE_PRE_ROLL_SAMPLES *
                            sizeof(*s_mic_voice_pre_roll_storage)));
    return ESP_OK;
}

static void mic_adc_release_psram_audio_storage(void)
{
    if (s_mic_voice_pre_roll_storage != NULL) {
        heap_caps_free(s_mic_voice_pre_roll_storage);
        s_mic_voice_pre_roll_storage = NULL;
    }
}

static void mic_adc_release_task_stack(void)
{
    if (s_mic_adc_task_stack != NULL) {
        heap_caps_free(s_mic_adc_task_stack);
        s_mic_adc_task_stack = NULL;
        memset(&s_mic_adc_task_storage, 0, sizeof(s_mic_adc_task_storage));
    }
}

static void mic_adc_handle_pause_if_needed(adc_continuous_handle_t handle,
                                           bool *adc_started,
                                           mic_adc_window_t *window,
                                           mic_adc_pcm_converter_t *pcm_converter,
                                           mic_vad_t *vad,
                                           mic_adc_voice_stream_t *voice_stream,
                                           mic_adc_local_wake_t *local_wake)
{
    if (!mic_adc_current_task_is_owner("pause")) {
        return;
    }
    if (adc_started == NULL) {
        return;
    }
    if (!mic_adc_pause_requested()) {
        return;
    }

    /*
     * pause_request 只能由外部任务设置；真正停止 PCM/ADC 必须在 mic_adc_test_task
     * 本任务内完成，避免 ADC driver 锁跨任务 acquire/release。
     */
    mic_adc_log_state("Mic ADC pause request", *adc_started, voice_stream);
    mic_adc_log_heap("Mic ADC pause stage: before runtime reset");
    mic_adc_reset_runtime_state(window, pcm_converter, vad, voice_stream, local_wake);
    mic_adc_log_heap("Mic ADC pause stage: after runtime reset");
    if (*adc_started) {
        mic_adc_log_heap("Mic ADC pause stage: before adc_continuous_stop");
        esp_err_t stop_ret = adc_continuous_stop(handle);
        mic_adc_log_heap("Mic ADC pause stage: after adc_continuous_stop");
        if (stop_ret == ESP_OK) {
            *adc_started = false;
            s_mic_adc_started = false;
            xEventGroupClearBits(s_mic_adc_control_events, MIC_ADC_CONTROL_RUNNING_BIT);
        } else if (stop_ret == ESP_ERR_INVALID_STATE) {
            *adc_started = false;
            s_mic_adc_started = false;
            xEventGroupClearBits(s_mic_adc_control_events, MIC_ADC_CONTROL_RUNNING_BIT);
            ESP_LOGW(TAG, "ADC continuous stop skipped: already stopped");
        } else {
            ESP_LOGE(TAG, "ADC continuous stop failed: %s", esp_err_to_name(stop_ret));
        }
    } else {
        ESP_LOGW(TAG, "ADC continuous stop skipped: adc_started=0");
    }
    if (s_mic_adc_control_events != NULL) {
        xEventGroupSetBits(s_mic_adc_control_events, MIC_ADC_CONTROL_PAUSED_BIT);
    }
    mic_adc_log_state("Mic ADC paused for half-duplex playback", *adc_started, voice_stream);

    while (mic_adc_pause_requested()) {
        if (mic_adc_stop_requested()) {
            ESP_LOGD(TAG, "Mic ADC pause loop interrupted by reconnect stop request");
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    if (mic_adc_stop_requested()) {
        return;
    }

    if (s_mic_adc_control_events != NULL) {
        xEventGroupClearBits(s_mic_adc_control_events, MIC_ADC_CONTROL_PAUSED_BIT);
    }
    if (*adc_started) {
        ESP_LOGW(TAG, "ADC continuous resume skipped: adc_started=1");
        return;
    }
    mic_adc_log_heap("Mic ADC resume stage: before adc_continuous_start");
    esp_err_t ret = adc_continuous_start(handle);
    mic_adc_log_heap("Mic ADC resume stage: after adc_continuous_start");
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "ADC continuous resume failed: %s", esp_err_to_name(ret));
        vTaskDelay(pdMS_TO_TICKS(MIC_ADC_ERROR_RETRY_DELAY_MS));
        return;
    }
    *adc_started = true;
    s_mic_adc_started = true;
    xEventGroupSetBits(s_mic_adc_control_events, MIC_ADC_CONTROL_RUNNING_BIT);
    mic_adc_log_heap("Mic ADC resume stage: before runtime reset");
    mic_adc_reset_runtime_state(window, pcm_converter, vad, voice_stream, local_wake);
    mic_adc_log_heap("Mic ADC resume stage: after runtime reset");
    mic_adc_log_state("Mic ADC resumed for listening", *adc_started, voice_stream);
}

static esp_err_t mic_adc_start_if_needed(adc_continuous_handle_t handle,
                                         bool *adc_started,
                                         mic_adc_window_t *window,
                                         mic_adc_pcm_converter_t *pcm_converter,
                                         mic_vad_t *vad,
                                         mic_adc_voice_stream_t *voice_stream,
                                         mic_adc_local_wake_t *local_wake,
                                         const char *reason)
{
    if (!mic_adc_current_task_is_owner("start")) {
        return ESP_ERR_INVALID_STATE;
    }
    if (handle == NULL || adc_started == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (*adc_started) {
        return ESP_OK;
    }
    if (mic_adc_pause_requested()) {
        return ESP_ERR_INVALID_STATE;
    }
    if (mic_adc_stop_requested()) {
        return ESP_ERR_INVALID_STATE;
    }

    mic_adc_log_start_stage("mic_adc_start", "before", ESP_ERR_NOT_FINISHED);
    esp_err_t ret = adc_continuous_start(handle);
    mic_adc_log_start_stage("mic_adc_start", "after", ret);
    if (ret != ESP_OK) {
        mic_adc_log_start_failure("adc_continuous_start", ret);
        ESP_LOGE(TAG,
                 "ADC continuous start failed: reason=%s ret=%s task=%s adc_started=%d pause_request=%d free_heap=%u",
                 reason != NULL ? reason : "<none>",
                 esp_err_to_name(ret),
                 mic_adc_task_name(),
                 *adc_started ? 1 : 0,
                 mic_adc_pause_requested() ? 1 : 0,
                 (unsigned int)esp_get_free_heap_size());
        return ret;
    }

    *adc_started = true;
    s_mic_adc_started = true;
    xEventGroupSetBits(s_mic_adc_control_events, MIC_ADC_CONTROL_RUNNING_BIT);
    mic_adc_log_start_stage("vad_start", "before", ESP_ERR_NOT_FINISHED);
    mic_adc_reset_runtime_state(window, pcm_converter, vad, voice_stream, local_wake);
    mic_adc_log_start_stage("vad_start", "after", ESP_OK);
    mic_adc_log_state(reason != NULL ? reason : "Mic ADC started", *adc_started, voice_stream);
    return ESP_OK;
}

static bool mic_adc_handle_stop_if_requested(adc_continuous_handle_t handle,
                                             bool *adc_started,
                                             mic_adc_window_t *window,
                                             mic_adc_pcm_converter_t *pcm_converter,
                                             mic_vad_t *vad,
                                             mic_adc_voice_stream_t *voice_stream,
                                             mic_adc_local_wake_t *local_wake)
{
    if (!mic_adc_current_task_is_owner("stop")) {
        return false;
    }
    if (!mic_adc_stop_requested()) {
        return false;
    }
    if (adc_started == NULL) {
        return false;
    }

    mic_adc_log_state("Mic ADC stop/deinit stage: task stop request", *adc_started, voice_stream);
    mic_adc_log_heap("Mic ADC stop/deinit stage: before runtime reset");
    mic_adc_reset_runtime_state(window, pcm_converter, vad, voice_stream, local_wake);
    mic_adc_clear_static_audio_storage();
    mic_adc_log_heap("Mic ADC stop/deinit stage: after runtime reset/audio clear");

    if (*adc_started) {
        mic_adc_log_heap("Mic ADC stop/deinit stage: before adc_continuous_stop");
        esp_err_t stop_ret = adc_continuous_stop(handle);
        mic_adc_log_heap("Mic ADC stop/deinit stage: after adc_continuous_stop");
        if (stop_ret == ESP_OK || stop_ret == ESP_ERR_INVALID_STATE) {
            *adc_started = false;
            s_mic_adc_started = false;
            xEventGroupClearBits(s_mic_adc_control_events, MIC_ADC_CONTROL_RUNNING_BIT);
            if (stop_ret == ESP_ERR_INVALID_STATE) {
                ESP_LOGW(TAG, "ADC continuous stop skipped during reconnect: already stopped");
            }
        } else {
            ESP_LOGE(TAG, "ADC continuous stop failed during reconnect: %s", esp_err_to_name(stop_ret));
            vTaskDelay(pdMS_TO_TICKS(MIC_ADC_ERROR_RETRY_DELAY_MS));
            return false;
        }
    } else {
        s_mic_adc_started = false;
        xEventGroupClearBits(s_mic_adc_control_events, MIC_ADC_CONTROL_RUNNING_BIT);
    }

    EventGroupHandle_t events = s_mic_adc_control_events;
    s_mic_adc_task_handle = NULL;
    if (events != NULL) {
        xEventGroupClearBits(events, MIC_ADC_CONTROL_PAUSED_BIT);
        xEventGroupClearBits(events, MIC_ADC_CONTROL_RUNNING_BIT);
        xEventGroupSetBits(events, MIC_ADC_CONTROL_STOPPED_BIT);
    }
    mic_adc_local_wake_deinit(local_wake);
    vTaskDelete(NULL);
    return true;
}

/**
 * @brief 轮询当前语音流后端是否已经完成收尾。
 *
 * 调用方法：Mic 本地状态处于 FINISHING 时调用。只有后端已经回到 idle，
 * 本地才打印 session done 并重新接收下一轮 pre-roll。
 *
 * @param stream server voice 流式状态，不能为空。
 * @return 已回到等待态返回 true；底层仍忙返回 false。
 */
static bool mic_adc_voice_stream_poll_finish(mic_adc_voice_stream_t *stream)
{
    if (stream == NULL || stream->state != MIC_ADC_VOICE_STATE_FINISHING) {
        return false;
    }
    if (!mic_adc_voice_backend_is_idle()) {
        return false;
    }

    mic_adc_voice_stream_enter_waiting(stream, true);
    return true;
}

/**
 * @brief 把预缓存按时间顺序发送给当前语音流后端。
 *
 * 调用方法：mic_adc_voice_stream_activate() 在 VAD 检测到 VOICE_START 且 server voice start
 * 成功后调用。函数开头再次检查状态，只有 STREAMING 才允许调用
 * 后端 append_pcm()，确保 POST_ROLL/FINISHING 期间不会继续补发句首缓存。
 *
 * @param stream server voice 流式状态，不能为空。
 * @return 成功返回 ESP_OK；发送失败返回错误码。
 */
static esp_err_t mic_adc_voice_stream_send_pre_roll(mic_adc_voice_stream_t *stream)
{
    if (stream == NULL || stream->state != MIC_ADC_VOICE_STATE_STREAMING ||
        stream->pre_roll_samples == NULL ||
        stream->pre_roll_sample_count == 0) {
        return ESP_OK;
    }
    if (stream->live_chunk_capacity_samples == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    size_t start_index = 0;
    if (stream->pre_roll_sample_count == stream->pre_roll_capacity_samples) {
        start_index = stream->pre_roll_write_index;
    }

    size_t remaining = stream->pre_roll_sample_count;
    size_t read_index = start_index;
    while (remaining > 0) {
        size_t contiguous = stream->pre_roll_capacity_samples - read_index;
        if (contiguous > remaining) {
            contiguous = remaining;
        }

        while (contiguous > 0) {
            size_t chunk_samples = contiguous;
            if (chunk_samples > stream->live_chunk_capacity_samples) {
                chunk_samples = stream->live_chunk_capacity_samples;
            }

            esp_err_t ret = mic_adc_voice_backend_append_pcm(&stream->pre_roll_samples[read_index],
                                                             chunk_samples);
            if (ret != ESP_OK) {
                return ret;
            }
            stream->pcm_bytes_total += (uint32_t)(chunk_samples * sizeof(int16_t));

            read_index += chunk_samples;
            if (read_index >= stream->pre_roll_capacity_samples) {
                read_index = 0;
            }
            remaining -= chunk_samples;
            contiguous -= chunk_samples;
        }
    }

    return ESP_OK;
}

/**
 * @brief 在 VAD VOICE_START 时打开 server voice PCM 发送闸门。
 *
 * 调用方法：mic_adc_window_report() 检测到 MIC_VAD_EVENT_VOICE_START 后调用。
 * 本函数先请求 voice_chain 进入语音独占并准备 server voice turn；只有回调成功后才
 * 打开 PCM 发送闸门并发送句首 pre-roll。server voice prepare 失败时保持 VAD-only，不推 PCM。
 *
 * @param stream server voice 流式状态，不能为空。
 * @return STREAMING 表示已进入 PCM 发送；DEFERRED 表示已进入本地提示/准备窗口；NONE 表示未启动。
 */
static mic_adc_voice_activate_result_t mic_adc_voice_stream_activate(mic_adc_voice_stream_t *stream)
{
    if (stream == NULL) {
        return MIC_ADC_VOICE_ACTIVATE_NONE;
    }
    if (stream->state == MIC_ADC_VOICE_STATE_FINISHING) {
        if (mic_adc_voice_stream_poll_finish(stream)) {
            stream->finish_busy_log_printed = false;
        } else {
            if (!stream->finish_busy_log_printed) {
                if (APP_DEBUG_VOICE_SESSION_LOG) {
                    ESP_LOGI(TAG, "server voice busy finishing previous turn");
                }
                stream->finish_busy_log_printed = true;
            }
            return MIC_ADC_VOICE_ACTIVATE_NONE;
        }
    }
    if (stream->state != MIC_ADC_VOICE_STATE_IDLE) {
        return MIC_ADC_VOICE_ACTIVATE_NONE;
    }
    if (!mic_adc_voice_stream_can_start(stream)) {
        return MIC_ADC_VOICE_ACTIVATE_NONE;
    }
    if (!gateway_link_can_start_voice_turn()) {
        mic_adc_voice_stream_delay_next_start(stream);
        stream->live_chunk_sample_count = 0;
        mic_adc_voice_stream_clear_pre_roll(stream);
        return MIC_ADC_VOICE_ACTIVATE_NONE;
    }
    if (!wifi_is_connected() || !wifi_is_stable()) {
        ESP_LOGW(TAG, "server voice: WiFi is not connected/stable, skip turn start");
        mic_adc_voice_stream_delay_next_start(stream);
        stream->live_chunk_sample_count = 0;
        mic_adc_voice_stream_clear_pre_roll(stream);
        return MIC_ADC_VOICE_ACTIVATE_NONE;
    }
    ESP_LOGI(TAG,
             "VOICE_START detected, request voice exclusive before %s PCM",
             mic_adc_voice_stream_name());
    stream->waiting_log_printed = false;
    esp_err_t ret = mic_adc_prepare_voice_exclusive();
    if (ret == ESP_ERR_NOT_FINISHED) {
        ESP_LOGI(TAG,
                 "%s prepare deferred recording, stay VAD-only",
                 mic_adc_voice_stream_name());
        mic_adc_voice_stream_clear_pre_roll(stream);
        mic_adc_voice_stream_enter_waiting(stream, false);
        return MIC_ADC_VOICE_ACTIVATE_DEFERRED;
    }
    if (ret != ESP_OK) {
        ESP_LOGW(TAG,
                 "%s prepare failed before PCM, stay VAD-only: %s",
                 mic_adc_voice_stream_name(),
                 esp_err_to_name(ret));
        mic_adc_voice_stream_delay_next_start(stream);
        mic_adc_voice_stream_enter_waiting(stream, false);
        return MIC_ADC_VOICE_ACTIVATE_NONE;
    }

    ESP_LOGI(TAG, "voice stream start using backend=%s", mic_adc_voice_stream_name());
    stream->state = MIC_ADC_VOICE_STATE_STREAMING;
    stream->session_start_tick = xTaskGetTickCount();
    stream->pcm_bytes_total = 0;
    stream->finish_busy_log_printed = false;
    stream->live_chunk_sample_count = 0;
    ret = mic_adc_voice_stream_send_pre_roll(stream);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG,
                 "%s send pre-roll failed: %s",
                 mic_adc_voice_stream_name(),
                 esp_err_to_name(ret));
        mic_adc_voice_stream_delay_next_start(stream);
        mic_adc_voice_stream_enter_waiting(stream, false);
        return MIC_ADC_VOICE_ACTIVATE_NONE;
    }

    mic_adc_voice_stream_clear_pre_roll(stream);
    return MIC_ADC_VOICE_ACTIVATE_STREAMING;
}

/**
 * @brief 发送实时小批量缓冲中的 PCM。
 *
 * 调用方法：STREAMING/POST_ROLL 状态下 live_chunk 凑满时调用。它只减少
 * send_pcm() 调用次数；server voice upload 复用这条小缓冲路径。
 *
 * @param stream server voice 流式状态，不能为空。
 * @return 成功返回 ESP_OK；发送失败返回错误码。
 */
static esp_err_t mic_adc_voice_stream_flush_live_chunk(mic_adc_voice_stream_t *stream)
{
    if (stream == NULL ||
        (stream->state != MIC_ADC_VOICE_STATE_STREAMING &&
         stream->state != MIC_ADC_VOICE_STATE_POST_ROLL) ||
        stream->live_chunk_sample_count == 0) {
        return ESP_OK;
    }

    size_t sent_samples = stream->live_chunk_sample_count;
    esp_err_t ret = mic_adc_voice_backend_append_pcm(stream->live_chunk_samples,
                                                     sent_samples);
    if (ret == ESP_OK) {
        stream->pcm_bytes_total += (uint32_t)(sent_samples * sizeof(int16_t));
        stream->live_chunk_sample_count = 0;
    }
    return ret;
}

static void mic_adc_voice_stream_finish(mic_adc_voice_stream_t *stream);

/**
 * @brief 向 server voice 流式链路追加一个 PCM 样本。
 *
 * 调用方法：每得到一个 int16_t PCM 样本就调用。
 * - IDLE：只写入 500 ms 小环形 pre-roll。
 * - STREAMING：累计 live_chunk，凑满后调用 server voice append。
 * - POST_ROLL：继续发送 VOICE_END 后的尾部 PCM，达到配置时长后再 finalize。
 * - FINISHING：直接丢弃样本，禁止继续发送 pre-roll 和 PCM。
 *
 * @param stream server voice 流式状态，不能为空。
 * @param pcm_sample int16_t PCM 样本。
 */
static void mic_adc_voice_stream_push_sample(mic_adc_voice_stream_t *stream, int16_t pcm_sample)
{
    if (stream == NULL) {
        return;
    }

    if (stream->state == MIC_ADC_VOICE_STATE_FINISHING &&
        !mic_adc_voice_stream_poll_finish(stream)) {
        return;
    }

    if (stream->state == MIC_ADC_VOICE_STATE_IDLE) {
        mic_adc_voice_stream_push_pre_roll(stream, pcm_sample);
        return;
    }
    if (stream->state != MIC_ADC_VOICE_STATE_STREAMING &&
        stream->state != MIC_ADC_VOICE_STATE_POST_ROLL) {
        return;
    }

    if (stream->live_chunk_samples == NULL ||
        stream->live_chunk_capacity_samples == 0) {
        return;
    }

    stream->live_chunk_samples[stream->live_chunk_sample_count] = pcm_sample;
    stream->live_chunk_sample_count++;
    bool post_roll_complete = false;
    if (stream->state == MIC_ADC_VOICE_STATE_POST_ROLL &&
        stream->post_roll_remaining_samples > 0) {
        stream->post_roll_remaining_samples--;
        post_roll_complete = stream->post_roll_remaining_samples == 0;
    }

    if (stream->live_chunk_sample_count < stream->live_chunk_capacity_samples &&
        !post_roll_complete) {
        return;
    }

    esp_err_t ret = mic_adc_voice_stream_flush_live_chunk(stream);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG,
                 "%s send PCM failed: %s",
                 mic_adc_voice_stream_name(),
                 esp_err_to_name(ret));
        mic_adc_voice_stream_delay_next_start(stream);
        mic_adc_voice_stream_enter_waiting(stream, true);
        return;
    }

    if (post_roll_complete) {
        mic_adc_voice_stream_finish(stream);
    }
}

/**
 * @brief 发送完 post-roll 后结束本轮 server voice turn。
 *
 * 调用方法：post-roll 到达配置时长后调用；post-roll 为 0 时由 VOICE_END 直接调用。
 * 函数先把 live_chunk 中不足 10 ms 的尾部 PCM 推入当前语音流后端；
 * 随后进入 FINISHING，因此 ADC 循环期间再来的样本不会继续发送 pre-roll 或 PCM。
 * finish() 会通知 server_voice_client 结束 PCM 上传并等待服务器裸 PCM 响应；
 * voice_chain 随后统一释放本轮语音资源并恢复 VAD/BME 待机。
 *
 * @param stream server voice 流式状态，不能为空。
 */
static void mic_adc_voice_stream_finish(mic_adc_voice_stream_t *stream)
{
    if (stream == NULL ||
        (stream->state != MIC_ADC_VOICE_STATE_STREAMING &&
         stream->state != MIC_ADC_VOICE_STATE_POST_ROLL)) {
        return;
    }

    esp_err_t ret = mic_adc_voice_stream_flush_live_chunk(stream);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "server voice flush tail PCM failed: %s", esp_err_to_name(ret));
    }
    stream->post_roll_remaining_samples = 0;
    stream->state = MIC_ADC_VOICE_STATE_FINISHING;
    stream->finish_busy_log_printed = false;

    ret = mic_adc_voice_backend_finish();
    if (ret == ESP_ERR_NOT_FOUND) {
        if (APP_DEBUG_VOICE_SESSION_LOG) {
            ESP_LOGI(TAG, "server voice turn no response yet");
        }
    } else if (ret != ESP_OK) {
        ESP_LOGW(TAG,
                 "%s finish failed: %s",
                 mic_adc_voice_stream_name(),
                 esp_err_to_name(ret));
        mic_adc_voice_stream_delay_next_start(stream);
    }

    if (!mic_adc_voice_stream_poll_finish(stream)) {
        if (APP_DEBUG_VOICE_SESSION_LOG) {
            ESP_LOGI(TAG, "server voice waiting for backend idle before turn done");
        }
    }
}

static void mic_adc_voice_stream_start_post_roll(mic_adc_voice_stream_t *stream)
{
    if (stream == NULL || stream->state != MIC_ADC_VOICE_STATE_STREAMING) {
        return;
    }

    stream->post_roll_remaining_samples = MIC_ADC_VOICE_POST_ROLL_SAMPLES;
    if (stream->post_roll_remaining_samples == 0) {
        mic_adc_voice_stream_finish(stream);
        return;
    }

    stream->state = MIC_ADC_VOICE_STATE_POST_ROLL;
    if (APP_DEBUG_VOICE_VAD_KEY_LOG) {
        ESP_LOGI(TAG,
                 "server voice: voice_end detected, sending post-roll pcm_ms=%d samples=%u",
                 MIC_ADC_VOICE_POST_ROLL_MS,
                 (unsigned int)stream->post_roll_remaining_samples);
    }
}

/**
 * @brief 计算一个窗口内的 AC RMS。
 *
 * 调用方法：mic_adc_window_report() 分别计算 ADC raw 和 PCM RMS 时调用。
 * 公式为 sqrt(E[x^2] - E[x]^2)，直接使用 sum 和 sum_square，避免旧算法因整数舍入得到 0。
 *
 * @param count 样本数量。
 * @param sum 样本累加和，可以为负数。
 * @param sum_square 样本平方累加和。
 * @return 去直流后的 RMS。
 */
static uint32_t mic_adc_calc_ac_rms(uint32_t count, int64_t sum, uint64_t sum_square)
{
    if (count == 0) {
        return 0;
    }

    uint64_t sample_count = count;
    uint64_t rms_denominator = sample_count * sample_count;
    uint64_t mean_square_scaled = sample_count * sum_square;
    uint64_t avg_square_scaled = (uint64_t)(sum * sum);

    if (mean_square_scaled <= avg_square_scaled) {
        return 0;
    }

    uint64_t variance = (mean_square_scaled - avg_square_scaled + rms_denominator / 2) /
                        rms_denominator;
    return mic_adc_isqrt_u64(variance);
}

/**
 * @brief 重置一个 Mic ADC 统计窗口。
 *
 * 调用方法：任务开始时调用一次；每次串口输出一行统计结果后再次调用。
 *
 * @param window 要重置的统计窗口，不能为空。
 */
static void mic_adc_window_reset(mic_adc_window_t *window)
{
    window->count = 0;
    window->min_raw = UINT32_MAX;
    window->max_raw = 0;
    window->last_raw = 0;
    window->sum_raw = 0;
    window->sum_square_raw = 0;
    window->adc_clip_low = 0;
    window->adc_clip_high = 0;
    window->min_pcm = INT16_MAX;
    window->max_pcm = INT16_MIN;
    window->last_pcm = 0;
    window->sum_pcm = 0;
    window->sum_square_pcm = 0;
    window->pcm_clip_low = 0;
    window->pcm_clip_high = 0;
}

/**
 * @brief 向当前统计窗口追加一个有效 ADC raw 样本。
 *
 * 调用方法：mic_adc_test_task() 解析到目标 ADC1_CH5 的有效样本，并完成
 * ADC -> PCM 转换后调用。
 *
 * @param window 当前统计窗口，不能为空。
 * @param raw ADC continuous 驱动解析出的原始采样值。
 * @param pcm 由 mic_adc_pcm_convert_sample() 转出的 int16_t PCM 样本。
 */
static void mic_adc_window_add(mic_adc_window_t *window, uint32_t raw, int16_t pcm)
{
    window->count++;
    window->last_raw = raw;
    window->sum_raw += raw;
    window->sum_square_raw += (uint64_t)raw * raw;
    window->last_pcm = pcm;
    window->sum_pcm += pcm;
    int32_t pcm_i32 = pcm;
    window->sum_square_pcm += (uint64_t)(pcm_i32 * pcm_i32);

    if (raw < window->min_raw) {
        window->min_raw = raw;
    }
    if (raw > window->max_raw) {
        window->max_raw = raw;
    }
    if (raw == 0) {
        window->adc_clip_low++;
    }
    if (raw >= MIC_ADC_PCM_ADC_RAW_MAX) {
        window->adc_clip_high++;
    }
    if (pcm < window->min_pcm) {
        window->min_pcm = pcm;
    }
    if (pcm > window->max_pcm) {
        window->max_pcm = pcm;
    }
    if (pcm == INT16_MIN) {
        window->pcm_clip_low++;
    }
    if (pcm == INT16_MAX) {
        window->pcm_clip_high++;
    }
}

/**
 * @brief 计算当前统计窗口的 Mic 指标，并驱动 VAD 和 server voice 流程。
 *
 * 调用方法：mic_adc_test_task() 累计到 MIC_ADC_REPORT_SAMPLES 个有效样本后调用。
 * 本函数只维护 VAD 和 server voice 流式发送，不再使用整句 PCM 缓存。
 *
 * @param window 已累计完成的统计窗口。
 * @param vad VAD 状态机，不能为空。
 * @param voice_stream server voice 流式状态，不能为空。
 */
static void mic_adc_window_report(const mic_adc_window_t *window,
                                  mic_vad_t *vad,
                                  mic_adc_voice_stream_t *voice_stream)
{
    if (window->count == 0 || vad == NULL || voice_stream == NULL) {
        return;
    }

    uint32_t adc_rms = mic_adc_calc_ac_rms(window->count,
                                           (int64_t)window->sum_raw,
                                           window->sum_square_raw);
    uint32_t adc_p2p = window->max_raw - window->min_raw;
    uint32_t pcm_rms = mic_adc_calc_ac_rms(window->count,
                                           window->sum_pcm,
                                           window->sum_square_pcm);
    uint32_t pcm_p2p = (uint32_t)((int32_t)window->max_pcm - (int32_t)window->min_pcm);
    int32_t min_pcm_i32 = (int32_t)window->min_pcm;
    int32_t max_pcm_i32 = (int32_t)window->max_pcm;
    uint32_t min_pcm_abs = min_pcm_i32 < 0 ? (uint32_t)(-min_pcm_i32) : (uint32_t)min_pcm_i32;
    uint32_t max_pcm_abs = max_pcm_i32 < 0 ? (uint32_t)(-max_pcm_i32) : (uint32_t)max_pcm_i32;
    uint32_t pcm_peak = min_pcm_abs > max_pcm_abs ? min_pcm_abs : max_pcm_abs;
    // clipped 是总削顶标记：ADC 或 PCM 任意方向发生削顶，都输出 1。
    uint32_t clipped = (window->adc_clip_low > 0 ||
                        window->adc_clip_high > 0 ||
                        window->pcm_clip_low > 0 ||
                        window->pcm_clip_high > 0) ? 1 : 0;
    mic_vad_features_t vad_features = {
        .adc_rms = adc_rms,
        .adc_p2p = adc_p2p,
        .pcm_rms = pcm_rms,
        .pcm_p2p = pcm_p2p,
        .pcm_peak = pcm_peak,
        .clipped = clipped,
    };
    mic_vad_event_t vad_event = mic_vad_process(vad, &vad_features);

    if (vad_event == MIC_VAD_EVENT_VOICE_START) {
        if (voice_stream->state == MIC_ADC_VOICE_STATE_FINISHING) {
            (void)mic_adc_voice_stream_poll_finish(voice_stream);
        }

        if (!local_wake_word_is_recording_window_open()) {
            ESP_LOGD(TAG,
                     "VAD voice_start ignored before fixed wake word rms=%u peak=%u",
                     (unsigned int)pcm_rms,
                     (unsigned int)pcm_peak);
            mic_adc_voice_stream_clear_pre_roll(voice_stream);
            mic_vad_init(vad);
            return;
        }

        TickType_t now = xTaskGetTickCount();
        uint32_t cooldown_elapsed_ms = 0;
        if (mic_adc_voice_stream_start_in_cooldown(voice_stream, now, &cooldown_elapsed_ms)) {
            if (voice_stream->last_cooldown_log_tick == 0 ||
                (int32_t)(now - voice_stream->last_cooldown_log_tick) >=
                    (int32_t)pdMS_TO_TICKS(1000)) {
                voice_stream->last_cooldown_log_tick = now;
                ESP_LOGD(TAG,
                         "VAD ignored during cooldown elapsed_ms=%u cooldown_ms=%u",
                         (unsigned int)cooldown_elapsed_ms,
                         (unsigned int)MIC_ADC_VOICE_START_COOLDOWN_MS);
            }
            mic_adc_voice_stream_clear_pre_roll(voice_stream);
            mic_vad_init(vad);
            return;
        }

        // Server voice 阶段采用边采样边发送：这里只打开 PCM 发送闸门并发送句首小预缓存，不再启动整句 PCM 缓存。
        if (mic_adc_voice_stream_activate(voice_stream) == MIC_ADC_VOICE_ACTIVATE_STREAMING) {
            ESP_LOGI(TAG, "USER_SPEECH_START");
            ESP_LOGD(TAG,
                     "vad_start rms=%u peak=%u hit_frames=%u cooldown_elapsed=%u",
                     (unsigned int)pcm_rms,
                     (unsigned int)pcm_peak,
                     (unsigned int)MIC_VAD_START_FRAMES,
                     (unsigned int)cooldown_elapsed_ms);
        }
    } else if (vad_event == MIC_VAD_EVENT_VOICE_END) {
        ESP_LOGI(TAG, "USER_SPEECH_END");
        // 句尾先进入 post-roll 继续发送尾部 PCM，再由 server voice 流式状态机 finalize。
        mic_adc_voice_stream_start_post_roll(voice_stream);
        mic_vad_init(vad);
    }
}

/**
 * @brief 打印 mic_adc_test 任务栈剩余水位。
 *
 * 调用方法：只有 MIC_ADC_ENABLE_STACK_DEBUG_LOG 打开时才会在任务启动后和采集循环中调用，
 * 用于临时观察任务栈余量；正式 server voice 流程默认关闭，避免串口被诊断日志占用。
 */
#if MIC_ADC_ENABLE_STACK_DEBUG_LOG
static void mic_adc_log_stack_high_water_mark(void)
{
    app_stack_monitor_log(TAG, "mic_adc_test", "periodic");
}
#endif

/**
 * @brief 初始化 ADC continuous 驱动并绑定 Mic 所在的 ADC1_CH5。
 *
 * 调用方法：仅由 mic_adc_test_start() 调用一次；成功后通过 out_handle 返回驱动句柄。
 *
 * @param out_handle 输出参数，用于保存初始化完成的 ADC continuous 句柄。
 * @return 成功返回 ESP_OK，失败返回 ESP-IDF 错误码。
 */
static esp_err_t mic_adc_continuous_init(adc_continuous_handle_t *out_handle)
{
    adc_continuous_handle_t handle = NULL;
    adc_continuous_handle_cfg_t handle_cfg = {
        .max_store_buf_size = MIC_ADC_STORE_BYTES,
        .conv_frame_size = MIC_ADC_READ_BYTES,
        .flags.flush_pool = true,
    };
    mic_adc_log_start_stage("mic_adc_dma_channel_create", "before", ESP_ERR_NOT_FINISHED);
    esp_err_t ret = adc_continuous_new_handle(&handle_cfg, &handle);
    mic_adc_log_start_stage("mic_adc_dma_channel_create", "after", ret);
    if (ret != ESP_OK) {
        mic_adc_log_start_failure("adc_continuous_new_handle", ret);
        return ret;
    }

    adc_digi_pattern_config_t pattern = {
        .atten = MIC_ADC_ATTEN,
        // ESP-IDF 示例要求 pattern channel 只保留低 3 bit；GPIO6/ADC1_CH5 保持为 5。
        .channel = MIC_ADC_CHANNEL & 0x7,
        .unit = MIC_ADC_UNIT,
        .bit_width = MIC_ADC_BIT_WIDTH,
    };

    adc_continuous_config_t adc_cfg = {
        .pattern_num = 1,
        .adc_pattern = &pattern,
        .sample_freq_hz = MIC_ADC_SAMPLE_FREQ_HZ,
        .conv_mode = MIC_ADC_CONV_MODE,
        .format = MIC_ADC_OUTPUT_FORMAT,
    };

    mic_adc_log_start_stage("mic_adc_channel_config", "before", ESP_ERR_NOT_FINISHED);
    ret = adc_continuous_config(handle, &adc_cfg);
    mic_adc_log_start_stage("mic_adc_channel_config", "after", ret);
    if (ret != ESP_OK) {
        mic_adc_log_start_failure("adc_continuous_config", ret);
        adc_continuous_deinit(handle);
        mic_adc_log_heap("Mic ADC init stage: after adc_continuous_deinit failed config");
        return ret;
    }

    *out_handle = handle;
    return ESP_OK;
}

/**
 * @brief Mic ADC continuous 采样和串口统计任务。
 *
 * 调用方法：由 mic_adc_test_start() 在 WiFi 稳定后预创建；任务先等待初始化 READY gate。
 * 不要在外部直接调用。
 * 任务流程：
 * 1. 从 ADC continuous 驱动读取 DMA 原始帧。
 * 2. 调用 adc_continuous_parse_data() 解析出 ADC 单元、通道和 raw 值。
 * 3. 只保留 GPIO6 对应的 ADC1_CH5 有效样本。
 * 4. 调用独立的 mic_adc_pcm 模块把 raw 转成 16000 Hz/mono/int16/little-endian PCM。
 * 5. 空闲监听期先把 PCM 喂给本地 WakeNet，只有“你好小智”命中才请求 voice_chain。
 * 6. 唤醒提示后继续用 VAD 捕捉用户后续语音，并打开 server voice PCM 发送闸门。
 *
 * @param arg 未使用；ADC handle 仅在 READY gate 打开后从已初始化的全局状态读取。
 */
static void mic_adc_test_task(void *arg)
{
    (void)arg;
    s_mic_adc_owner_task = xTaskGetCurrentTaskHandle();
    EventGroupHandle_t events = s_mic_adc_control_events;
    EventBits_t init_bits = xEventGroupWaitBits(events,
                                                MIC_CTRL_INIT_READY |
                                                    MIC_CTRL_INIT_ABORT |
                                                    MIC_ADC_CONTROL_STOP_REQUEST_BIT,
                                                pdFALSE,
                                                pdFALSE,
                                                portMAX_DELAY);
    if ((init_bits & (MIC_CTRL_INIT_ABORT | MIC_ADC_CONTROL_STOP_REQUEST_BIT)) != 0) {
        if (events == s_mic_adc_control_events) {
            xEventGroupSetBits(events, MIC_ADC_CONTROL_STOPPED_BIT);
        }
        s_mic_adc_task_handle = NULL;
        s_mic_adc_owner_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    adc_continuous_handle_t handle = s_adc_handle;
    mic_adc_log_heap("Mic ADC task stage: entry");
    app_stack_monitor_log(TAG, "mic_adc_test", "entry");
    mic_adc_window_t window;
    mic_adc_pcm_converter_t pcm_converter;
    bool adc_started = false;
    s_mic_adc_started = false;
#if MIC_ADC_ENABLE_STACK_DEBUG_LOG
    TickType_t last_stack_log_tick = 0;
#endif

    mic_adc_pcm_converter_init(&pcm_converter);
    mic_adc_window_reset(&window);
    mic_adc_log_heap("Mic ADC task stage: after local state init");
    app_stack_monitor_log(TAG, "mic_adc_test", "after_local_state_init");
    if (APP_DEBUG_VOICE_SESSION_LOG) {
        ESP_LOGI(TAG,
                 "mic_adc_test task started, server voice input is signed int16 little-endian PCM converted by mic_adc_pcm_convert_sample(), not raw ADC values");
    }
#if MIC_ADC_ENABLE_STACK_DEBUG_LOG
    mic_adc_log_stack_high_water_mark();
    last_stack_log_tick = xTaskGetTickCount();
#endif

    while (1) {
        mic_adc_handle_pause_if_needed(handle,
                                       &adc_started,
                                       &window,
                                       &pcm_converter,
                                       &s_mic_adc_vad,
                                       &s_mic_adc_voice_stream,
                                       &s_mic_adc_local_wake);
        if (mic_adc_handle_stop_if_requested(handle,
                                             &adc_started,
                                             &window,
                                             &pcm_converter,
                                             &s_mic_adc_vad,
                                             &s_mic_adc_voice_stream,
                                             &s_mic_adc_local_wake)) {
            continue;
        }

        if (!adc_started) {
            esp_err_t start_ret = mic_adc_start_if_needed(handle,
                                                          &adc_started,
                                                          &window,
                                                          &pcm_converter,
                                                          &s_mic_adc_vad,
                                                          &s_mic_adc_voice_stream,
                                                          &s_mic_adc_local_wake,
                                                          "Mic ADC started by mic_adc_test_task");
            if (start_ret != ESP_OK) {
                vTaskDelay(pdMS_TO_TICKS(MIC_ADC_ERROR_RETRY_DELAY_MS));
                continue;
            }
        }

#if MIC_ADC_ENABLE_STACK_DEBUG_LOG
        TickType_t now = xTaskGetTickCount();
        if ((now - last_stack_log_tick) >= pdMS_TO_TICKS(1000)) {
            last_stack_log_tick = now;
            mic_adc_log_stack_high_water_mark();
        }
#endif

        uint32_t read_bytes = 0;
        esp_err_t ret = adc_continuous_read(handle,
                                            s_mic_adc_raw_buffer,
                                            sizeof(s_mic_adc_raw_buffer),
                                            &read_bytes,
                                            MIC_ADC_READ_TIMEOUT_MS);
        if (ret == ESP_ERR_TIMEOUT) {
#if MIC_ADC_ENABLE_LOOP_DEBUG_LOG
            ESP_LOGW(TAG, "ADC read timeout");
#endif
            continue;
        }
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "ADC read failed: %s", esp_err_to_name(ret));
            vTaskDelay(pdMS_TO_TICKS(MIC_ADC_ERROR_RETRY_DELAY_MS));
            continue;
        }

        if (mic_adc_handle_stop_if_requested(handle,
                                             &adc_started,
                                             &window,
                                             &pcm_converter,
                                             &s_mic_adc_vad,
                                             &s_mic_adc_voice_stream,
                                             &s_mic_adc_local_wake)) {
            continue;
        }

        uint32_t sample_count = 0;
        ret = adc_continuous_parse_data(handle,
                                        s_mic_adc_raw_buffer,
                                        read_bytes,
                                        s_mic_adc_parsed_buffer,
                                        &sample_count);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "ADC parse failed: %s", esp_err_to_name(ret));
            continue;
        }

        for (uint32_t i = 0; i < sample_count; i++) {
            if (mic_adc_stop_requested()) {
                break;
            }
            if (!s_mic_adc_parsed_buffer[i].valid ||
                s_mic_adc_parsed_buffer[i].unit != MIC_ADC_UNIT ||
                s_mic_adc_parsed_buffer[i].channel != MIC_ADC_CHANNEL) {
                continue;
            }

            int16_t pcm_sample = mic_adc_pcm_convert_sample(&pcm_converter,
                                                            s_mic_adc_parsed_buffer[i].raw_data);
            if (mic_adc_local_wake_push_idle_sample(&s_mic_adc_local_wake,
                                                    &s_mic_adc_voice_stream,
                                                    &s_mic_adc_vad,
                                                    pcm_sample)) {
                mic_adc_window_reset(&window);
                break;
            }
            // Server voice 采用流式发送：未触发 VAD 时写入小预缓存，触发后边采样边发送 PCM。
            mic_adc_voice_stream_push_sample(&s_mic_adc_voice_stream, pcm_sample);
            mic_adc_window_add(&window, s_mic_adc_parsed_buffer[i].raw_data, pcm_sample);
            if (window.count >= MIC_ADC_REPORT_SAMPLES) {
                mic_adc_window_report(&window, &s_mic_adc_vad, &s_mic_adc_voice_stream);
                mic_adc_window_reset(&window);
            }
        }
    }
}

static void mic_adc_abort_precreated_task(void)
{
    EventGroupHandle_t events = s_mic_adc_control_events;
    if (events == NULL || s_mic_adc_task_handle == NULL) {
        return;
    }

    mic_adc_log_start_stage("mic_adc_task_init_abort", "before", ESP_ERR_NOT_FINISHED);
    xEventGroupClearBits(events, MIC_ADC_CONTROL_STOPPED_BIT);
    xEventGroupSetBits(events, MIC_CTRL_INIT_ABORT);
    (void)xEventGroupWaitBits(events,
                              MIC_ADC_CONTROL_STOPPED_BIT,
                              pdFALSE,
                              pdFALSE,
                              portMAX_DELAY);
    mic_adc_log_start_stage("mic_adc_task_init_abort", "after", ESP_OK);
    s_mic_adc_task_handle = NULL;
}

static void mic_adc_cleanup_failed_start(void)
{
    mic_adc_abort_precreated_task();

    /* The READY gate is not opened on this path, so the owner never started ADC. */
    s_mic_adc_started = false;
    mic_adc_local_wake_deinit(&s_mic_adc_local_wake);
    memset(&s_mic_adc_vad, 0, sizeof(s_mic_adc_vad));
    memset(&s_mic_adc_voice_stream, 0, sizeof(s_mic_adc_voice_stream));

    if (s_adc_handle != NULL) {
        mic_adc_log_heap("Mic ADC failed start: before adc_continuous_deinit");
        esp_err_t deinit_ret = adc_continuous_deinit(s_adc_handle);
        mic_adc_log_heap("Mic ADC failed start: after adc_continuous_deinit");
        if (deinit_ret != ESP_OK && deinit_ret != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG, "ADC continuous deinit failed during startup rollback: %s", esp_err_to_name(deinit_ret));
        }
        s_adc_handle = NULL;
    }
    if (s_mic_adc_control_events != NULL) {
        vEventGroupDelete(s_mic_adc_control_events);
        s_mic_adc_control_events = NULL;
    }
    mic_adc_clear_static_audio_storage();
    mic_adc_release_psram_audio_storage();
    mic_adc_release_task_stack();
}

/**
 * @brief 启动 Mic ADC continuous 采样测试。
 *
 * 调用方法：voice_chain 在 WiFi 已连接稳定后调用一次。Mic 常驻 VAD-only 待机；
 * VAD 触发 VOICE_START 时通过注册回调请求 voice_chain 暂停非语音模块并准备 server voice。
 *
 * @return 成功返回 ESP_OK，失败返回 ESP-IDF 错误码。
 */
esp_err_t mic_adc_test_start(void)
{
    mic_adc_log_heap("Mic ADC start API: enter");
    if (s_mic_adc_task_handle != NULL) {
        mic_adc_log_heap("Mic ADC start API: reuse existing task");
        return ESP_OK;
    }

    if (s_mic_adc_control_events == NULL) {
        mic_adc_log_start_stage("mic_control_event_group_create", "before", ESP_ERR_NOT_FINISHED);
        s_mic_adc_control_events = xEventGroupCreate();
        if (s_mic_adc_control_events == NULL) {
            mic_adc_log_start_stage("mic_control_event_group_create", "after", ESP_ERR_NO_MEM);
            mic_adc_log_start_failure("xEventGroupCreate", ESP_ERR_NO_MEM);
            return ESP_ERR_NO_MEM;
        }
        mic_adc_log_start_stage("mic_control_event_group_create", "after", ESP_OK);
    }
    mic_adc_log_heap("Mic ADC start API: before control event clear");
    xEventGroupClearBits(s_mic_adc_control_events,
                         MIC_ADC_CONTROL_PAUSE_REQUEST_BIT |
                             MIC_ADC_CONTROL_PAUSED_BIT |
                             MIC_ADC_CONTROL_STOP_REQUEST_BIT |
                             MIC_ADC_CONTROL_STOPPED_BIT |
                             MIC_ADC_CONTROL_RUNNING_BIT |
                             MIC_CTRL_INIT_READY |
                             MIC_CTRL_INIT_ABORT);
    mic_adc_log_heap("Mic ADC start API: after control event clear");

    mic_adc_log_heap("Mic ADC start API: before wifi ready check");
    if (!wifi_is_stable()) {
        ESP_LOGW(TAG, "WiFi is not stable yet, skip Mic ADC/VAD start");
        return ESP_ERR_INVALID_STATE;
    }
    mic_adc_log_heap("Mic ADC start API: after wifi ready check");

    mic_adc_log_start_stage("mic_pre_roll_alloc", "before", ESP_ERR_NOT_FINISHED);
    esp_err_t ret = mic_adc_allocate_psram_audio_storage();
    mic_adc_log_start_stage("mic_pre_roll_alloc", "after", ret);
    if (ret != ESP_OK) {
        mic_adc_log_start_failure("heap_caps_malloc(mic_pre_roll)", ret);
        mic_adc_cleanup_failed_start();
        return ret;
    }

    mic_adc_log_start_stage("mic_adc_task_create", "before", ESP_ERR_NOT_FINISHED);
    ESP_LOGI(TAG,
             "VOICE_START_STAGE stage=mic_adc_task_create requested_stack_bytes=%u source=psram",
             (unsigned int)MIC_ADC_TEST_TASK_STACK_SIZE);
    s_mic_adc_task_stack = heap_caps_malloc(MIC_ADC_TEST_TASK_STACK_WORDS *
                                                 sizeof(*s_mic_adc_task_stack),
                                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_mic_adc_task_stack == NULL) {
        mic_adc_log_start_stage("mic_adc_task_create", "after", ESP_ERR_NO_MEM);
        mic_adc_log_start_failure("heap_caps_malloc(mic_adc_task_stack)", ESP_ERR_NO_MEM);
        mic_adc_cleanup_failed_start();
        return ESP_ERR_NO_MEM;
    }
    s_mic_adc_task_handle = xTaskCreateStatic(mic_adc_test_task,
                                               "mic_adc_test",
                                               MIC_ADC_TEST_TASK_STACK_WORDS,
                                               NULL,
                                               MIC_ADC_TASK_PRIORITY,
                                               s_mic_adc_task_stack,
                                               &s_mic_adc_task_storage);
    if (s_mic_adc_task_handle == NULL) {
        mic_adc_log_start_stage("mic_adc_task_create", "after", ESP_ERR_NO_MEM);
        mic_adc_log_start_failure("xTaskCreateStatic(mic_adc_test)", ESP_ERR_NO_MEM);
        s_mic_adc_task_handle = NULL;
        mic_adc_cleanup_failed_start();
        return ESP_ERR_NO_MEM;
    }
    mic_adc_log_start_stage("mic_adc_task_create", "after", ESP_OK);
    ESP_LOGI(TAG,
             "VOICE_TASK_STACK task=mic_adc_test bytes=%u source=psram_static",
             (unsigned int)MIC_ADC_TEST_TASK_STACK_SIZE);

    mic_adc_log_start_stage("mic_adc_init", "before", ESP_ERR_NOT_FINISHED);
    ret = mic_adc_continuous_init(&s_adc_handle);
    mic_adc_log_start_stage("mic_adc_init", "after", ret);
    if (ret != ESP_OK) {
        mic_adc_log_start_failure("mic_adc_continuous_init", ret);
        ESP_LOGE(TAG, "ADC continuous init failed: %s", esp_err_to_name(ret));
        mic_adc_cleanup_failed_start();
        return ret;
    }

    mic_adc_log_start_stage("vad_init", "before", ESP_ERR_NOT_FINISHED);
    mic_vad_init(&s_mic_adc_vad);
    mic_adc_log_start_stage("vad_init", "after", ESP_OK);
    mic_adc_voice_stream_init(&s_mic_adc_voice_stream,
                              s_mic_voice_pre_roll_storage,
                              MIC_ADC_VOICE_PRE_ROLL_SAMPLES,
                              s_mic_voice_live_chunk_storage,
                              MIC_ADC_VOICE_LIVE_CHUNK_SAMPLES);
    ret = mic_adc_local_wake_init(&s_mic_adc_local_wake);
    if (ret != ESP_OK) {
        mic_adc_log_start_failure("mic_adc_local_wake_init", ret);
        ESP_LOGE(TAG, "local WakeNet feed init failed: %s", esp_err_to_name(ret));
        mic_adc_cleanup_failed_start();
        return ret;
    }

    /* The Mic task starts ADC after this gate; callers only create/request it. */
    s_mic_adc_started = false;
    xEventGroupSetBits(s_mic_adc_control_events, MIC_CTRL_INIT_READY);

    if (APP_DEBUG_VOICE_SESSION_LOG) {
        ESP_LOGI(TAG,
                 "Mic ADC task created: OPA_OUT -> GPIO%d/ADC1_CH%d, sample_rate=%d Hz",
                 MIC_ADC_GPIO_NUM,
                 (int)MIC_ADC_CHANNEL,
                 MIC_ADC_SAMPLE_FREQ_HZ);
    }
    return ESP_OK;
}

void mic_adc_test_set_voice_stream_ops(const mic_adc_voice_stream_ops_t *ops)
{
    if (ops == NULL) {
        memset(&s_mic_adc_voice_stream_ops, 0, sizeof(s_mic_adc_voice_stream_ops));
        s_mic_adc_voice_stream_ops_registered = false;
        ESP_LOGD(TAG, "Mic voice stream ops cleared; server voice backend required");
        return;
    }

    s_mic_adc_voice_stream_ops = *ops;
    s_mic_adc_voice_stream_ops_registered = true;
    ESP_LOGD(TAG,
             "Mic voice stream ops registered backend=%s",
             mic_adc_voice_stream_name());
}

esp_err_t mic_adc_test_pause(void)
{
    mic_adc_log_heap("Mic ADC pause API: enter");
    if (s_mic_adc_control_events == NULL || s_mic_adc_task_handle == NULL) {
        mic_adc_log_heap("Mic ADC pause API: no task");
        return ESP_OK;
    }
    mic_adc_log_heap("Mic ADC pause API: before set pause bit");
    xEventGroupSetBits(s_mic_adc_control_events, MIC_ADC_CONTROL_PAUSE_REQUEST_BIT);
    mic_adc_log_heap("Mic ADC pause API: after set pause bit");
    return ESP_OK;
}

esp_err_t mic_adc_test_resume(void)
{
    mic_adc_log_heap("Mic ADC resume API: enter");
    if (s_mic_adc_control_events == NULL || s_mic_adc_task_handle == NULL) {
        mic_adc_log_heap("Mic ADC resume API: no task");
        return ESP_OK;
    }
    mic_adc_log_heap("Mic ADC resume API: VAD-only standby");
    mic_adc_log_heap("Mic ADC resume API: before clear pause bit");
    xEventGroupClearBits(s_mic_adc_control_events, MIC_ADC_CONTROL_PAUSE_REQUEST_BIT);
    mic_adc_log_heap("Mic ADC resume API: after clear pause bit");
    return ESP_OK;
}

esp_err_t mic_adc_test_wait_paused(uint32_t timeout_ms)
{
    mic_adc_log_heap("Mic ADC wait paused API: enter");
    if (s_mic_adc_control_events == NULL || s_mic_adc_task_handle == NULL) {
        mic_adc_log_heap("Mic ADC wait paused API: no task");
        return ESP_OK;
    }

    EventBits_t bits = xEventGroupWaitBits(s_mic_adc_control_events,
                                           MIC_ADC_CONTROL_PAUSED_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           pdMS_TO_TICKS(timeout_ms));
    mic_adc_log_heap("Mic ADC wait paused API: after wait");
    return (bits & MIC_ADC_CONTROL_PAUSED_BIT) != 0 ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t mic_adc_test_stop_and_deinit_for_reconnect(uint32_t timeout_ms)
{
    mic_adc_log_heap("Mic ADC stop/deinit API: enter");
    if (s_mic_adc_control_events == NULL || s_mic_adc_task_handle == NULL) {
        mic_adc_log_heap("Mic ADC stop/deinit API: no running task");
        mic_adc_local_wake_deinit(&s_mic_adc_local_wake);
        memset(&s_mic_adc_vad, 0, sizeof(s_mic_adc_vad));
        memset(&s_mic_adc_voice_stream, 0, sizeof(s_mic_adc_voice_stream));
        if (s_adc_handle != NULL) {
            mic_adc_log_heap("Mic ADC stop/deinit API: before adc continuous deinit no task");
            esp_err_t deinit_ret = adc_continuous_deinit(s_adc_handle);
            mic_adc_log_heap("Mic ADC stop/deinit API: after adc continuous deinit no task");
            if (deinit_ret != ESP_OK && deinit_ret != ESP_ERR_INVALID_STATE) {
                ESP_LOGE(TAG,
                         "ADC continuous deinit failed without task: %s",
                         esp_err_to_name(deinit_ret));
                return deinit_ret;
            }
            s_adc_handle = NULL;
        }
        if (s_mic_adc_control_events != NULL) {
            mic_adc_log_heap("Mic ADC stop/deinit API: before event group delete no task");
            vEventGroupDelete(s_mic_adc_control_events);
            s_mic_adc_control_events = NULL;
            mic_adc_log_heap("Mic ADC stop/deinit API: after event group delete no task");
        }
        s_mic_adc_started = false;
        s_mic_adc_task_handle = NULL;
        mic_adc_clear_static_audio_storage();
        mic_adc_release_psram_audio_storage();
        mic_adc_release_task_stack();
        mic_adc_log_heap("Mic ADC stop/deinit API: done no task");
        return ESP_OK;
    }

    mic_adc_log_heap("Mic ADC stop/deinit API: before set stop request");
    xEventGroupClearBits(s_mic_adc_control_events, MIC_ADC_CONTROL_STOPPED_BIT);
    xEventGroupSetBits(s_mic_adc_control_events, MIC_ADC_CONTROL_STOP_REQUEST_BIT);
    mic_adc_log_heap("Mic ADC stop/deinit API: after set stop request");

    EventBits_t bits = xEventGroupWaitBits(s_mic_adc_control_events,
                                           MIC_ADC_CONTROL_STOPPED_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           pdMS_TO_TICKS(timeout_ms));
    mic_adc_log_heap("Mic ADC stop/deinit API: after wait stopped");
    if ((bits & MIC_ADC_CONTROL_STOPPED_BIT) == 0) {
        ESP_LOGE(TAG,
                 "Mic ADC stop/deinit timeout during voice recovery: timeout_ms=%u",
                 (unsigned int)timeout_ms);
        return ESP_ERR_TIMEOUT;
    }

    /* 任务可能仍在 READY gate 等待，停止确认后统一回收预初始化的状态。 */
    mic_adc_local_wake_deinit(&s_mic_adc_local_wake);
    memset(&s_mic_adc_vad, 0, sizeof(s_mic_adc_vad));
    memset(&s_mic_adc_voice_stream, 0, sizeof(s_mic_adc_voice_stream));

    if (s_adc_handle != NULL) {
        mic_adc_log_heap("Mic ADC stop/deinit API: before adc continuous deinit");
        esp_err_t deinit_ret = adc_continuous_deinit(s_adc_handle);
        mic_adc_log_heap("Mic ADC stop/deinit API: after adc continuous deinit");
        if (deinit_ret != ESP_OK && deinit_ret != ESP_ERR_INVALID_STATE) {
            ESP_LOGE(TAG,
                     "ADC continuous deinit failed during voice recovery: %s",
                     esp_err_to_name(deinit_ret));
            return deinit_ret;
        }
        s_adc_handle = NULL;
    }

    mic_adc_log_heap("Mic ADC stop/deinit API: before event group delete");
    vEventGroupDelete(s_mic_adc_control_events);
    s_mic_adc_control_events = NULL;
    mic_adc_log_heap("Mic ADC stop/deinit API: after event group delete");

    s_mic_adc_task_handle = NULL;
    s_mic_adc_started = false;
    mic_adc_clear_static_audio_storage();
    mic_adc_release_psram_audio_storage();
    mic_adc_release_task_stack();
    mic_adc_log_heap("Mic ADC stop/deinit API: done");
    return ESP_OK;
}

esp_err_t mic_adc_test_release_for_speaker(uint32_t timeout_ms)
{
    return mic_adc_test_stop_and_deinit_for_reconnect(timeout_ms);
}

esp_err_t mic_adc_test_wait_running(uint32_t timeout_ms)
{
    if (s_mic_adc_control_events == NULL || s_mic_adc_task_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    EventBits_t bits = xEventGroupWaitBits(s_mic_adc_control_events,
                                           MIC_ADC_CONTROL_RUNNING_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           pdMS_TO_TICKS(timeout_ms));
    return (bits & MIC_ADC_CONTROL_RUNNING_BIT) != 0 ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t mic_adc_test_clear_audio_cache(void)
{
    mic_adc_log_heap("Mic ADC cleanup stage: before audio cache clear");
    if (s_mic_adc_task_handle != NULL && !mic_adc_test_is_paused()) {
        ESP_LOGW(TAG, "Mic audio cache cleanup rejected: Mic ADC is not paused");
        mic_adc_log_heap("Mic ADC cleanup stage: audio cache clear rejected");
        return ESP_ERR_INVALID_STATE;
    }

    mic_adc_clear_static_audio_storage();
    mic_adc_log_heap("Mic ADC cleanup stage: after audio cache clear");
    ESP_LOGD(TAG, "Mic server voice audio cache cleared");
    return ESP_OK;
}

bool mic_adc_test_is_paused(void)
{
    if (s_mic_adc_control_events == NULL) {
        return false;
    }
    EventBits_t bits = xEventGroupGetBits(s_mic_adc_control_events);
    return (bits & MIC_ADC_CONTROL_PAUSED_BIT) != 0;
}

uint32_t mic_adc_test_get_session_generation(void)
{
    return s_mic_session_generation;
}
