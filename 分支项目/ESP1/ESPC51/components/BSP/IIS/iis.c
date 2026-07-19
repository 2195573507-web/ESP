#include "iis.h"

/**
 * @file iis.c
 * @brief ESP32-C5 I2S PDM TX BSP 实现。
 *
 * 本文件只处理硬件侧：I2S/PDM channel 创建、PCM2PDM 参数校验、PA 使能、
 * start/write/stop 边界。PCM 分包、重采样和播放任务调度留在 speaker 层。
 */

#include <stdbool.h>

#include "app_debug_config.h"
#include "driver/gpio.h"
#include "driver/i2s_pdm.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "soc/soc_caps.h"

#if !SOC_I2S_SUPPORTS_PDM_TX
#error "IIS BSP requires I2S PDM TX support"
#endif

#if !SOC_I2S_SUPPORTS_PCM2PDM
#error "IIS BSP requires hardware PCM2PDM support"
#endif

static const char *TAG = "bsp_iis";

/* I2S/PDM TX channel 由 BSP 独占持有，上层不要直接访问 ESP-IDF handle。 */
static i2s_chan_handle_t s_tx_chan = NULL;
static bool s_tx_enabled = false;

/* 保护 init/start/stop 状态切换，避免播放任务与初始化路径并发操作 channel。 */
static SemaphoreHandle_t s_iis_mutex = NULL;
static bool s_iis_config_logged = false;

static void iis_log_dma_heap_before_init(void)
{
    const unsigned int slot_num = IIS_PDM_SLOT_MODE == I2S_SLOT_MODE_MONO ? 1U : 2U;
    const unsigned int bytes_per_sample = sizeof(int16_t);
    const unsigned int bytes_per_dma_buffer =
        IIS_EFFECTIVE_DMA_FRAME_NUM * slot_num * bytes_per_sample;
    const unsigned int estimated_total_dma_bytes =
        IIS_EFFECTIVE_DMA_DESC_NUM * bytes_per_dma_buffer;
    ESP_LOGI(TAG,
             "IIS_DMA_REQUIREMENT dma_desc_num=%u dma_frame_num=%u slot_num=%u "
             "data_bit_width=%u bytes_per_sample=%u bytes_per_dma_buffer=%u "
             "estimated_total_dma_bytes=%u dma_free=%u dma_largest=%u "
             "internal_free=%u internal_largest=%u",
             (unsigned int)IIS_EFFECTIVE_DMA_DESC_NUM,
             (unsigned int)IIS_EFFECTIVE_DMA_FRAME_NUM,
             slot_num,
             (unsigned int)IIS_BITS_PER_SAMPLE,
             bytes_per_sample,
             bytes_per_dma_buffer,
             estimated_total_dma_bytes,
             (unsigned int)heap_caps_get_free_size(MALLOC_CAP_DMA),
             (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_DMA),
             (unsigned int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned int)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
}

static void iis_log_values(const char *message,
                           int64_t value0,
                           int64_t value1,
                           int64_t value2,
                           int64_t value3)
{
#if APP_DEBUG_BSP_IIS_LOG
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

static esp_err_t iis_ensure_mutex(void)
{
    if (s_iis_mutex != NULL) {
        return ESP_OK;
    }

    s_iis_mutex = xSemaphoreCreateMutex();
    return s_iis_mutex == NULL ? ESP_ERR_NO_MEM : ESP_OK;
}

static unsigned long iis_calc_pdm_clock_hz(const i2s_pdm_tx_clk_config_t *clk_cfg)
{
    /*
     * ESP-IDF 的 PDM clock 由 sample_rate * 64 * (up_sample_fp / up_sample_fs) 推出。
     * 这里用于配置前自检，避免参数改动后静默落到错误频率。
     */
    if (clk_cfg == NULL || clk_cfg->up_sample_fs == 0) {
        return 0;
    }

    unsigned long long numerator =
        (unsigned long long)clk_cfg->sample_rate_hz * 64ULL * clk_cfg->up_sample_fp;
    return (unsigned long)(numerator / clk_cfg->up_sample_fs);
}

static int iis_pa_disabled_level(void)
{
    return IIS_PA_ENABLE_LEVEL ? 0 : 1;
}

static esp_err_t iis_pa_set_enabled(bool enabled, const char *reason)
{
    if (IIS_GPIO_PA_CTL == GPIO_NUM_NC) {
        ESP_LOGD(TAG,
                 "speaker PA control skipped reason=%s gpio=GPIO_NUM_NC",
                 reason != NULL ? reason : "<none>");
        return ESP_OK;
    }

    int level = enabled ? IIS_PA_ENABLE_LEVEL : iis_pa_disabled_level();
    esp_err_t err = gpio_set_level(IIS_GPIO_PA_CTL, level);
    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "gpio_set_level(PA_CTL gpio=%d level=%d reason=%s) failed: %s",
                 (int)IIS_GPIO_PA_CTL,
                 level,
                 reason != NULL ? reason : "<none>",
                 esp_err_to_name(err));
        return err;
    }

    ESP_LOGD(TAG,
             "speaker PA %s reason=%s gpio=%d level=%d readback=%d active_level=%d",
             enabled ? "enabled" : "disabled",
             reason != NULL ? reason : "<none>",
             (int)IIS_GPIO_PA_CTL,
             level,
             gpio_get_level(IIS_GPIO_PA_CTL),
             IIS_PA_ENABLE_LEVEL);
    return ESP_OK;
}

static esp_err_t iis_pa_init(void)
{
    /* PA 控制脚可选；硬件没有独立功放使能脚时定义为 GPIO_NUM_NC 即可跳过。 */
    if (IIS_GPIO_PA_CTL == GPIO_NUM_NC) {
        ESP_LOGD(TAG, "speaker PA control gpio=GPIO_NUM_NC");
        return ESP_OK;
    }

    esp_err_t err = gpio_set_direction(IIS_GPIO_PA_CTL, GPIO_MODE_OUTPUT);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_set_direction(PA_CTL) failed: %s", esp_err_to_name(err));
        return err;
    }

    return iis_pa_set_enabled(false, "init");
}

static void iis_log_hardware_config_once(void)
{
    if (s_iis_config_logged) {
        return;
    }
    s_iis_config_logged = true;
    ESP_LOGD(TAG,
             "speaker output config mode=PDM_TX_PCM2PDM port=%d sample_rate=%u bits=16 "
             "slot_mode=%s slot_mask=%d pdm_clk_gpio=%d pdm_data_gpio=%d pdm_data2_gpio=%d "
             "pa_gpio=%d pa_active_level=%d pa_settle_ms=%d std_i2s_bclk_ws_dout=not_used",
             (int)IIS_PORT,
             (unsigned int)IIS_SAMPLE_RATE_HZ,
             (IIS_PDM_SLOT_MODE == I2S_SLOT_MODE_MONO) ? "mono" : "stereo",
             (int)IIS_PDM_SLOT_MASK,
             (int)IIS_PDM_GPIO_CLK,
             (int)IIS_PDM_GPIO_DATA,
             (int)IIS_PDM_GPIO_DATA2,
             (int)IIS_GPIO_PA_CTL,
             IIS_PA_ENABLE_LEVEL,
             IIS_PA_ENABLE_SETTLE_MS);
}

static esp_err_t iis_validate_pdm_tx_config(const i2s_pdm_tx_config_t *cfg)
{
    /*
     * 集中校验最终 PDM TX 配置。后续如果调整 GPIO、采样率或 DMA 参数，
     * 这里能尽早发现“宏值已改但 driver 配置没跟上”的问题。
     */
    unsigned long pdm_clock_hz = iis_calc_pdm_clock_hz(&cfg->clk_cfg);

    if (cfg->clk_cfg.sample_rate_hz != IIS_SAMPLE_RATE_HZ ||
        cfg->clk_cfg.up_sample_fp != IIS_PDM_UPSAMPLE_FP ||
        cfg->clk_cfg.up_sample_fs != IIS_PDM_UPSAMPLE_FS ||
        pdm_clock_hz < IIS_PDM_CLOCK_LOW_LIMIT_HZ ||
        cfg->slot_cfg.data_bit_width != IIS_BITS_PER_SAMPLE ||
        cfg->slot_cfg.slot_mode != IIS_PDM_SLOT_MODE ||
        cfg->slot_cfg.data_fmt != I2S_PDM_DATA_FMT_PCM ||
        cfg->gpio_cfg.clk != IIS_PDM_GPIO_CLK ||
        cfg->gpio_cfg.dout != IIS_PDM_GPIO_DATA ||
        cfg->gpio_cfg.dout2 != IIS_PDM_GPIO_DATA2) {
        iis_log_values("invalid PDM TX config",
                       cfg->clk_cfg.sample_rate_hz,
                       cfg->clk_cfg.up_sample_fp,
                       cfg->clk_cfg.up_sample_fs,
                       pdm_clock_hz);
        return ESP_ERR_INVALID_ARG;
    }

#if SOC_I2S_HW_VERSION_2
    if (cfg->slot_cfg.line_mode != I2S_PDM_TX_ONE_LINE_CODEC) {
        iis_log_values("invalid PDM TX line_mode",
                       cfg->slot_cfg.line_mode,
                       I2S_PDM_TX_ONE_LINE_CODEC,
                       0,
                       0);
        return ESP_ERR_INVALID_ARG;
    }
#endif

    return ESP_OK;
}

esp_err_t iis_init(void)
{
    /*
     * 初始化只创建并配置 PDM TX channel，不 enable 输出。
     * 这样 app 启动时可以提前做硬件准备，真正播放时再由 iis_start() 打开 DMA。
     */
    esp_err_t err = iis_ensure_mutex();
    if (err != ESP_OK) {
        return err;
    }

    if (xSemaphoreTake(s_iis_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    if (s_tx_chan != NULL) {
        xSemaphoreGive(s_iis_mutex);
        return ESP_OK;
    }

    err = iis_pa_init();
    if (err != ESP_OK) {
        xSemaphoreGive(s_iis_mutex);
        return err;
    }

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(IIS_PORT, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = IIS_EFFECTIVE_DMA_DESC_NUM;
    chan_cfg.dma_frame_num = IIS_EFFECTIVE_DMA_FRAME_NUM;
    /* 自动清尾能减少停止/重启播放时 DMA 里残留上一段 PCM 的概率。 */
    chan_cfg.auto_clear_after_cb = true;
    chan_cfg.auto_clear_before_cb = false;

    iis_log_values("DMA config",
                   IIS_PORT,
                   chan_cfg.dma_desc_num,
                   chan_cfg.dma_frame_num,
                   chan_cfg.dma_desc_num * chan_cfg.dma_frame_num);

    iis_log_dma_heap_before_init();
    err = i2s_new_channel(&chan_cfg, &s_tx_chan, NULL);
    if (err != ESP_OK) {
        s_tx_chan = NULL;
        (void)iis_pa_set_enabled(false, "new_channel_fail");
        xSemaphoreGive(s_iis_mutex);
        return err;
    }

    i2s_pdm_tx_config_t pdm_tx_cfg = {
        .clk_cfg = I2S_PDM_TX_CLK_DAC_DEFAULT_CONFIG(IIS_SAMPLE_RATE_HZ),
        .slot_cfg = I2S_PDM_TX_SLOT_PCM_FMT_DEFAULT_CONFIG(IIS_BITS_PER_SAMPLE,
                                                           IIS_PDM_SLOT_MODE),
        .gpio_cfg = {
            .clk = IIS_PDM_GPIO_CLK,
            .dout = IIS_PDM_GPIO_DATA,
            .dout2 = IIS_PDM_GPIO_DATA2,
            .invert_flags = {
                .clk_inv = IIS_PDM_CLK_INVERT ? true : false,
            },
        },
    };

    /* 覆盖默认宏生成值，确保最终输出格式和 iis.h 对外声明保持一致。 */
    pdm_tx_cfg.clk_cfg.sample_rate_hz = IIS_SAMPLE_RATE_HZ;
    pdm_tx_cfg.clk_cfg.up_sample_fp = IIS_PDM_UPSAMPLE_FP;
    pdm_tx_cfg.clk_cfg.up_sample_fs = IIS_PDM_UPSAMPLE_FS;
    pdm_tx_cfg.slot_cfg.data_bit_width = IIS_BITS_PER_SAMPLE;
    pdm_tx_cfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_16BIT;

    iis_log_values("final PDM clock",
                   pdm_tx_cfg.clk_cfg.sample_rate_hz,
                   pdm_tx_cfg.clk_cfg.up_sample_fp,
                   pdm_tx_cfg.clk_cfg.up_sample_fs,
                   pdm_tx_cfg.clk_cfg.bclk_div);
    iis_log_values("final PDM GPIO",
                   pdm_tx_cfg.gpio_cfg.clk,
                   pdm_tx_cfg.gpio_cfg.dout,
                   pdm_tx_cfg.gpio_cfg.dout2,
                   (int)pdm_tx_cfg.gpio_cfg.invert_flags.clk_inv);

    err = iis_validate_pdm_tx_config(&pdm_tx_cfg);
    if (err != ESP_OK) {
        i2s_del_channel(s_tx_chan);
        s_tx_chan = NULL;
        (void)iis_pa_set_enabled(false, "validate_fail");
        xSemaphoreGive(s_iis_mutex);
        return err;
    }

    err = i2s_channel_init_pdm_tx_mode(s_tx_chan, &pdm_tx_cfg);
    if (err != ESP_OK) {
        i2s_del_channel(s_tx_chan);
        s_tx_chan = NULL;
        (void)iis_pa_set_enabled(false, "init_pdm_fail");
        xSemaphoreGive(s_iis_mutex);
        return err;
    }

    i2s_chan_info_t chan_info = {};
    err = i2s_channel_get_info(s_tx_chan, &chan_info);
    if (err != ESP_OK) {
        i2s_del_channel(s_tx_chan);
        s_tx_chan = NULL;
        (void)iis_pa_set_enabled(false, "get_info_fail");
        xSemaphoreGive(s_iis_mutex);
        return err;
    }

    iis_log_values("driver readback",
                   chan_info.is_enabled ? 1 : 0,
                   chan_info.sclk_hz,
                   chan_info.bclk_hz,
                   chan_info.total_dma_buf_size);

    /* driver readback 的 bclk_hz 过低时，PDM clock 可能不足，直接视为配置异常。 */
    if (chan_info.bclk_hz < IIS_PDM_CLOCK_LOW_LIMIT_HZ) {
        i2s_del_channel(s_tx_chan);
        s_tx_chan = NULL;
        (void)iis_pa_set_enabled(false, "bclk_check_fail");
        xSemaphoreGive(s_iis_mutex);
        return ESP_ERR_INVALID_ARG;
    }

    s_tx_enabled = false;
    ESP_LOGI(TAG, "SPEAKER_TX_INIT_OK");
    iis_log_hardware_config_once();
    ESP_LOGD(TAG,
             "IIS/PDM TX ready enabled=%d sclk=%u bclk=%u dma_total=%u",
             chan_info.is_enabled ? 1 : 0,
             (unsigned int)chan_info.sclk_hz,
             (unsigned int)chan_info.bclk_hz,
             (unsigned int)chan_info.total_dma_buf_size);
#if APP_DEBUG_BSP_IIS_LOG
    ESP_LOGI(TAG, "IIS PDM TX ready; channel stopped until playback starts");
#endif
    xSemaphoreGive(s_iis_mutex);
    return ESP_OK;
}

esp_err_t iis_start(void)
{
    /* iis_start() 可重复调用；已启动时直接返回，避免上层重复 enable channel。 */
    esp_err_t err = iis_init();
    if (err != ESP_OK) {
        return err;
    }

    if (xSemaphoreTake(s_iis_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    if (s_tx_enabled) {
        xSemaphoreGive(s_iis_mutex);
        return ESP_OK;
    }

    err = iis_pa_set_enabled(true, "iis_start_before_enable");
    if (err != ESP_OK) {
        xSemaphoreGive(s_iis_mutex);
        return err;
    }
    if (IIS_PA_ENABLE_SETTLE_MS > 0) {
        vTaskDelay(pdMS_TO_TICKS(IIS_PA_ENABLE_SETTLE_MS));
    }

    err = i2s_channel_enable(s_tx_chan);
    if (err == ESP_OK) {
        s_tx_enabled = true;
        ESP_LOGD(TAG, "IIS/PDM TX enabled port=%d sample_rate=%u", (int)IIS_PORT, (unsigned int)IIS_SAMPLE_RATE_HZ);
    } else {
        (void)iis_pa_set_enabled(false, "iis_start_enable_fail");
    }
    xSemaphoreGive(s_iis_mutex);
    return err;
}

esp_err_t iis_write(const void *data,
                    size_t bytes,
                    size_t *bytes_written,
                    uint32_t timeout_ms)
{
    /* 只允许在 channel 已初始化且已启动后写入，避免静默丢 PCM。 */
    if (bytes_written != NULL) {
        *bytes_written = 0;
    }
    if (data == NULL || bytes == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_tx_chan == NULL || !s_tx_enabled) {
        return ESP_ERR_INVALID_STATE;
    }

    return i2s_channel_write(s_tx_chan,
                             data,
                             bytes,
                             bytes_written,
                             pdMS_TO_TICKS(timeout_ms));
}

esp_err_t iis_stop(void)
{
    /* iis_stop() 可重复调用，未初始化或未启动都按成功处理，便于 cleanup 路径统一调用。 */
    if (s_iis_mutex == NULL) {
        return ESP_OK;
    }
    if (xSemaphoreTake(s_iis_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    if (s_tx_chan == NULL || !s_tx_enabled) {
        xSemaphoreGive(s_iis_mutex);
        return ESP_OK;
    }

    esp_err_t err = i2s_channel_disable(s_tx_chan);
    if (err == ESP_OK) {
        s_tx_enabled = false;
        ESP_LOGD(TAG, "IIS/PDM TX disabled port=%d", (int)IIS_PORT);
    }
    (void)iis_pa_set_enabled(false, "iis_stop");
    xSemaphoreGive(s_iis_mutex);
    return err;
}

esp_err_t iis_deinit_timed(uint32_t lock_timeout_ms)
{
    if (s_iis_mutex == NULL) {
        return ESP_OK;
    }
    TickType_t timeout_ticks = lock_timeout_ms == UINT32_MAX ?
                                   portMAX_DELAY : pdMS_TO_TICKS(lock_timeout_ms);
    if (lock_timeout_ms > 0U && timeout_ticks == 0U) {
        timeout_ticks = 1U;
    }
    if (xSemaphoreTake(s_iis_mutex, timeout_ticks) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t ret = ESP_OK;
    if (s_tx_chan != NULL) {
        if (s_tx_enabled) {
            esp_err_t disable_ret = i2s_channel_disable(s_tx_chan);
            if (disable_ret == ESP_OK || disable_ret == ESP_ERR_INVALID_STATE) {
                s_tx_enabled = false;
            } else {
                ret = disable_ret;
            }
        }

        esp_err_t del_ret = i2s_del_channel(s_tx_chan);
        if (ret == ESP_OK && del_ret != ESP_OK) {
            ret = del_ret;
        }
        s_tx_chan = NULL;
        s_tx_enabled = false;
    }
    (void)iis_pa_set_enabled(false, "iis_deinit");
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "SPEAKER_TX_DEINIT_OK");
    }

    xSemaphoreGive(s_iis_mutex);
    return ret;
}

esp_err_t iis_deinit(void)
{
    return iis_deinit_timed(UINT32_MAX);
}

esp_err_t iis_get_info(i2s_chan_info_t *out_info)
{
    if (out_info == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_tx_chan == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    return i2s_channel_get_info(s_tx_chan, out_info);
}

bool iis_is_started(void)
{
    return s_tx_enabled;
}
