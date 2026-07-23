#ifndef MIC_ADC_TEST_H
#define MIC_ADC_TEST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "hal/adc_types.h"
#include "soc/soc_caps.h"
#include "app_debug_config.h"

/* ESP-IDF has no generic ESP_ERR_NOT_READY.  Keep the temporary startup
 * condition distinct from an invalid lifecycle or an ADC hardware failure. */
#define MIC_ADC_ERR_NOT_READY ESP_ERR_NOT_FINISHED

/* 硬件连接：OPA_OUT -> ESP32-C5 GPIO6 / ADC1_CH5。 */
#define MIC_ADC_GPIO_NUM             6                         // Mic 输入 GPIO。
#define MIC_ADC_UNIT                 ADC_UNIT_1                // GPIO6 属于 ADC1。
#define MIC_ADC_CHANNEL              ADC_CHANNEL_5             // GPIO6 对应 ADC1_CH5。

/* ADC continuous 配置：只采一个 Mic 通道。 */
#define MIC_ADC_CONV_MODE            ADC_CONV_SINGLE_UNIT_1    // 只用 ADC1。
#define MIC_ADC_OUTPUT_FORMAT        ADC_DIGI_OUTPUT_FORMAT_TYPE2 // ESP32-C5 DMA 格式。
#define MIC_ADC_ATTEN                ADC_ATTEN_DB_12           // 输入衰减，量程更大。
#define MIC_ADC_BIT_WIDTH            SOC_ADC_DIGI_MAX_BITWIDTH // 使用芯片最大 ADC 位宽。
#define MIC_ADC_SAMPLE_FREQ_HZ       16000                     // 采样率，需等于 PCM 采样率。

/* ADC 读取和统计参数：调试刷新速度、缓冲和任务资源时改这里。 */
#define MIC_ADC_READ_BYTES            512  // 单次 ADC 读取字节数。
#define MIC_ADC_STORE_BYTES           4096 // ADC DMA 缓存大小。
#define MIC_ADC_REPORT_SAMPLES        (MIC_ADC_SAMPLE_FREQ_HZ / 5) // 约 200 ms 一帧 VAD。
#define MIC_ADC_READ_TIMEOUT_MS       1000 // ADC 读取超时。
#define MIC_ADC_ERROR_RETRY_DELAY_MS  100  // 异常后短暂退避。
#define MIC_ADC_TEST_TASK_STACK_SIZE  12288 // mic_adc_test 任务栈；ESP-IDF FreeRTOS 单位为字节。
#define MIC_ADC_TEST_TASK_STACK_WORDS \
    ((MIC_ADC_TEST_TASK_STACK_SIZE + sizeof(StackType_t) - 1U) / sizeof(StackType_t))
#define MIC_ADC_TASK_PRIORITY         4    // ADC 任务优先级。
#define MIC_ADC_ENABLE_LOOP_DEBUG_LOG APP_DEBUG_MIC_ADC_LOOP_LOG   // 循环普通日志总开关，错误日志不受影响。
#define MIC_ADC_ENABLE_STACK_DEBUG_LOG APP_DEBUG_MIC_ADC_STACK_LOG  // 任务栈水位诊断开关，server voice 稳定后默认关闭。

/* Server voice 流式发送参数：只保留小预缓存和小实时块，避免 voice turn 占用整句 PCM 缓存。 */
#define MIC_ADC_VOICE_PRE_ROLL_MS        (APP_VOICE_AUDIO_PACKET_MS * APP_VOICE_PRE_SPEECH_PACKETS) // VOICE_START 前保留 3~5 个 server voice 包的句首 PCM。
#define MIC_ADC_VOICE_PRE_ROLL_MAX_MS    1000 // 预缓存上限，防止静态 RAM 误增。
#define MIC_ADC_VOICE_PRE_ROLL_SAMPLES   ((MIC_ADC_SAMPLE_FREQ_HZ * MIC_ADC_VOICE_PRE_ROLL_MS) / 1000) // 预缓存样本数。
#define MIC_ADC_VOICE_POST_ROLL_MS       APP_VOICE_POST_ROLL_MS // VOICE_END 后继续发送的尾部 PCM 时长。
#define MIC_ADC_VOICE_POST_ROLL_SAMPLES  ((MIC_ADC_SAMPLE_FREQ_HZ * MIC_ADC_VOICE_POST_ROLL_MS) / 1000) // 尾部补偿样本数。
#define MIC_ADC_VOICE_LIVE_CHUNK_SAMPLES 160  // 实时发送块，160 samples = 10 ms PCM。
#define MIC_ADC_VOICE_RETRY_DELAY_MS      2000 // voice turn 启动失败后等待 2 秒再允许下一次启动，避免断网时疯狂重连。
#define MIC_ADC_VOICE_START_COOLDOWN_MS   APP_VOICE_VAD_START_COOLDOWN_MS // server voice done 后忽略短时间内的 VAD 起始抖动。

#if MIC_ADC_VOICE_PRE_ROLL_MS <= 0
#error "MIC_ADC_VOICE_PRE_ROLL_MS must be greater than 0"
#endif

#if MIC_ADC_VOICE_PRE_ROLL_MS > MIC_ADC_VOICE_PRE_ROLL_MAX_MS
#error "MIC_ADC_VOICE_PRE_ROLL_MS must not exceed MIC_ADC_VOICE_PRE_ROLL_MAX_MS"
#endif

#if MIC_ADC_VOICE_POST_ROLL_MS < 0 || MIC_ADC_VOICE_POST_ROLL_MS > 1000
#error "MIC_ADC_VOICE_POST_ROLL_MS must be between 0 and 1000"
#endif

#if MIC_ADC_VOICE_RETRY_DELAY_MS < 1000 || MIC_ADC_VOICE_RETRY_DELAY_MS > 3000
#error "MIC_ADC_VOICE_RETRY_DELAY_MS must be between 1000 and 3000"
#endif

/**
 * @brief 启动 Mic ADC continuous 采样任务。
 *
 * 硬件链路：外接模拟麦克风 -> 板上 Mic 前端/运放 -> OPA_OUT -> GPIO6/ADC1_CH5。
 * 调用方法：voice_chain 初始化 server_voice_client/speaker 后调用一次；重复调用会直接返回
 * ESP_OK。Mic 启动后常驻 VAD-only 待机，server voice turn 只在 VAD 触发说话后由
 * voice_chain 先暂停非语音模块再打开。
 *
 * 启动顺序必须保持为：
 * 1. WiFi 稳定后由 voice_chain 初始化 server_voice_client/speaker 和 voice 回调。
 * 2. 启动 ADC continuous，Mic 常驻 VAD-only 待机。
 * 3. ADC 任务采到 PCM 后先进入 IDLE，只维护句首 pre-roll。
 * 4. VAD 触发 VOICE_START 时先请求 voice_chain 进入语音独占并准备 server voice turn。
 * 5. 外层 VAD 触发 VOICE_END 后先发送 post-roll，再进入 FINISHING；
 *    finish/stop 完成本轮 session 后回到 IDLE，继续等待下一次说话。
 *
 * @return 成功返回 ESP_OK；WiFi 尚未稳定或前一轮收尾中返回
 * MIC_ADC_ERR_NOT_READY；硬件或资源错误返回对应 ESP-IDF 错误码。
 */
esp_err_t mic_adc_test_start(void);

typedef esp_err_t (*mic_adc_voice_append_pcm_cb_t)(const int16_t *pcm,
                                                   size_t samples,
                                                   void *user_ctx);
typedef esp_err_t (*mic_adc_voice_finish_cb_t)(void *user_ctx);
typedef bool (*mic_adc_voice_is_idle_cb_t)(void *user_ctx);
typedef bool (*mic_adc_voice_is_ready_cb_t)(void *user_ctx);

typedef struct {
    esp_err_t (*prepare_cb)(void *user_ctx);
    mic_adc_voice_append_pcm_cb_t append_pcm_cb;
    mic_adc_voice_finish_cb_t finish_cb;
    mic_adc_voice_is_idle_cb_t is_idle_cb;
    mic_adc_voice_is_ready_cb_t is_ready_cb;
    void *user_ctx;
    const char *stream_name;
} mic_adc_voice_stream_ops_t;

/**
 * @brief 注册 Mic VAD 后端语音流操作。
 *
 * 调用方法：voice_chain_start() 在启动 Mic ADC 前注册。server voice 模式下
 * append/finish 直接上传 PCM 到 local gateway。传 NULL 会清空自定义后端并保持
 * VAD-only 待机，不回退到任何本地云端客户端。
 *
 * @param ops 语音流操作表，可为 NULL。
 */
void mic_adc_test_set_voice_stream_ops(const mic_adc_voice_stream_ops_t *ops);

/**
 * @brief 请求暂停 Mic ADC 采样。
 *
 * 调用方法：半双工 voice 状态机准备播放服务器 PCM 前调用。函数只设置暂停请求，
 * 真正 stop ADC continuous 由 mic_adc_test 任务在安全点完成，避免跨任务直接操作
 * ADC 驱动句柄。
 *
 * @return Mic 任务已启动或尚未启动都返回 ESP_OK。
 */
esp_err_t mic_adc_test_pause(void);

/**
 * @brief 恢复 Mic ADC 采样。
 *
 * 调用方法：speaker 播放完成后由 voice 状态机调用。恢复后 Mic 任务会重置
 * VAD/server voice 小缓存并重新进入等待说话状态。
 *
 * @return 成功返回 ESP_OK。
 */
esp_err_t mic_adc_test_resume(void);

/**
 * @brief 等待 Mic 任务进入暂停态。
 *
 * @param timeout_ms 最大等待时间，0 表示只检查一次。
 * @return 已暂停返回 ESP_OK；超时返回 ESP_ERR_TIMEOUT。
 */
esp_err_t mic_adc_test_wait_paused(uint32_t timeout_ms);

/**
 * @brief 强制释放 Mic ADC 任务和外设资源。
 *
 * 调用方法：保留给异常恢复路径。函数会请求
 * mic_adc_test 任务在安全点退出，等待退出确认，然后 deinit adc_continuous handle、
 * 删除控制 event group，并清空内部 handle/task 指针。Mic 任务不存在时直接 no-op。
 * 普通半双工播放暂停仍使用 mic_adc_test_pause() + mic_adc_test_wait_paused()。
 *
 * @param timeout_ms 等待 Mic 任务退出的最大时间。
 * @return 释放完成返回 ESP_OK；任务未按时退出返回 ESP_ERR_TIMEOUT。
 */
esp_err_t mic_adc_test_stop_and_deinit_for_reconnect(uint32_t timeout_ms);

/**
 * @brief 在同一 voice lease 内安全退出 Mic task 并释放 ADC continuous/DMA。
 *
 * 调用方法：voice_chain 在 Speaker PDM TX 初始化前调用。该函数不触碰
 * C5 resource lease/generation；仅在 Mic task 已离开 ADC read 临界区后释放
 * ADC handle 和其 DMA 资源。成功后必须通过 mic_adc_test_start() 重建 Mic。
 */
esp_err_t mic_adc_test_release_for_speaker(uint32_t timeout_ms);

/**
 * @brief 等待 Mic task 已重建 ADC continuous 并实际开始采样。
 *
 * 调用方法：voice_chain 在 mic_adc_test_start() 后、打开正式录音窗口前调用。
 */
esp_err_t mic_adc_test_wait_running(uint32_t timeout_ms);

/**
 * @brief 清空 Mic server voice 本地小音频缓存。
 *
 * 调用方法：voice_chain 已请求暂停并确认 Mic ADC 任务处于 PAUSED 后调用。
 * 本函数只清 pre-roll/live chunk 静态存储和已暂停任务的本地状态；如果 Mic 仍在
 * 采样，返回 ESP_ERR_INVALID_STATE，避免边写 PCM 边清缓存。
 *
 * @return 清理完成返回 ESP_OK；Mic 未暂停时返回 ESP_ERR_INVALID_STATE。
 */
esp_err_t mic_adc_test_clear_audio_cache(void);

/**
 * @brief 查询 Mic ADC 是否已暂停。
 *
 * @return 已暂停返回 true。
 */
bool mic_adc_test_is_paused(void);

/** @brief 返回当前 Mic/VAD 录音周期 generation，用于丢弃旧异步事件。 */
uint32_t mic_adc_test_get_session_generation(void);

#endif // MIC_ADC_TEST_H
