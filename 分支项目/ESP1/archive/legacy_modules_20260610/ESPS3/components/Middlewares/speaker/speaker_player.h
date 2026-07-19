#ifndef SPEAKER_PLAYER_H
#define SPEAKER_PLAYER_H

#include <stdint.h>

#include "esp_err.h"
#include "iis.h"

/**
 * @file speaker_player.h
 * @brief speaker PCM 播放器。
 *
 * 输入格式：
 * 1. audio_player_play_pcm() 接收 PCM16 单声道 16 kHz。
 * 2. audio_player_play_16k_pcm() 保留兼容名，播放服务器返回的 PCM16 单声道 16 kHz。
 *
 * 职责边界：
 * 1. IIS/PDM GPIO、PA、DMA 底层归 BSP/IIS 管理。
 * 2. 本模块只负责把 PCM 切成固定音频块，经环形缓冲区交给写入任务写 IIS。
 */

/* 对外暴露的播放格式与 BSP/IIS 保持一致，避免上层重复包含 IIS 细节。 */
#define AUDIO_PLAYER_SAMPLE_RATE_HZ IIS_SAMPLE_RATE_HZ
#define AUDIO_PLAYER_BITS_PER_SAMPLE IIS_BITS_PER_SAMPLE
#define AUDIO_PLAYER_PDM_SLOT_MODE IIS_PDM_SLOT_MODE
#define AUDIO_PLAYER_PDM_SLOT_MASK IIS_PDM_SLOT_MASK
#define AUDIO_PLAYER_DMA_DESC_NUM IIS_DMA_DESC_NUM
#define AUDIO_PLAYER_DMA_FRAME_NUM IIS_DMA_FRAME_NUM
#define AUDIO_PLAYER_EFFECTIVE_DMA_DESC_NUM IIS_EFFECTIVE_DMA_DESC_NUM
#define AUDIO_PLAYER_EFFECTIVE_DMA_FRAME_NUM IIS_EFFECTIVE_DMA_FRAME_NUM

/* 播放器可调参数：调播放流缓存、写 IIS 任务资源和 DMA 诊断时改这里。 */
#ifndef AUDIO_PLAYER_REQUIRED_SAMPLE_RATE_HZ
#define AUDIO_PLAYER_REQUIRED_SAMPLE_RATE_HZ IIS_SAMPLE_RATE_HZ // speaker 原生播放采样率。
#endif

#ifndef AUDIO_PLAYER_DMA_STARVATION_THRESHOLD_US
#define AUDIO_PLAYER_DMA_STARVATION_THRESHOLD_US 20000LL // 单次 iis_write 超过该耗时打印告警。
#endif

#ifndef AUDIO_PLAYER_HEAP_MONITOR_INTERVAL_US
#define AUDIO_PLAYER_HEAP_MONITOR_INTERVAL_US 500000LL // 播放中 heap 诊断周期，单位 us。
#endif

#ifndef AUDIO_PLAYER_PCM_CHUNK_SAMPLES
#define AUDIO_PLAYER_PCM_CHUNK_SAMPLES 512U // 单个 ringbuffer 条目的 PCM 样本数。
#endif

#ifndef AUDIO_PLAYER_PCM_CHUNK_BYTES
#define AUDIO_PLAYER_PCM_CHUNK_BYTES (AUDIO_PLAYER_PCM_CHUNK_SAMPLES * sizeof(int16_t))
#endif

#ifndef AUDIO_PLAYER_RING_BUFFER_CHUNKS
#define AUDIO_PLAYER_RING_BUFFER_CHUNKS 4U // 播放环形缓冲区可容纳的音频块数。
#endif

#ifndef AUDIO_PLAYER_RING_BUFFER_SEND_TIMEOUT_MS
#define AUDIO_PLAYER_RING_BUFFER_SEND_TIMEOUT_MS 1000U // 投递 PCM 块等待 ringbuffer 的超时。
#endif

#ifndef AUDIO_PLAYER_I2S_WRITER_TASK_STACK_SIZE
#define AUDIO_PLAYER_I2S_WRITER_TASK_STACK_SIZE 8192U // IIS 写入任务栈，单位字节。
#endif

#ifndef AUDIO_PLAYER_I2S_WRITER_TASK_PRIORITY
#define AUDIO_PLAYER_I2S_WRITER_TASK_PRIORITY 6U // IIS 写入任务优先级。
#endif

#ifndef AUDIO_PLAYER_I2S_NONBLOCK_TIMEOUT_MS
#define AUDIO_PLAYER_I2S_NONBLOCK_TIMEOUT_MS 0U // iis_write 超时，0 表示非阻塞轮询。
#endif

#ifndef AUDIO_PLAYER_I2S_DMA_BACKOFF_TICKS
#define AUDIO_PLAYER_I2S_DMA_BACKOFF_TICKS 1U // DMA 暂不可写时的退避 tick 数。
#endif

#ifndef AUDIO_PLAYER_I2S_RETRY_LOG_INTERVAL
#define AUDIO_PLAYER_I2S_RETRY_LOG_INTERVAL 32U // DMA 重试多少次打印一次等待日志。
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 speaker 播放器和底层 IIS。
 *
 * 调用方法：系统启动或首次播放前调用；函数可重复调用。
 */
esp_err_t audio_player_init(void);

/**
 * @brief 播放 PCM16 单声道 16 kHz 数据。
 *
 * 调用方法：本地提示音或已是 16 kHz 的整段 PCM 调用；函数会阻塞到播放完成。
 *
 * @param data PCM 采样数组。
 * @param samples int16_t 采样点数量，不是字节数。
 */
esp_err_t audio_player_play_pcm(const int16_t *data, uint32_t samples);

/**
 * @brief 播放服务器 PCM 数据。
 *
 * 调用方法：服务器返回整段 PCM 时调用；当前 speaker/IIS 原生播放 16 kHz。
 *
 * @param data PCM 采样数组。
 * @param samples int16_t 采样点数量，不是字节数。
 * @param sample_rate_hz 输入采样率；当前支持 16 kHz。
 */
esp_err_t audio_player_play_16k_pcm(const int16_t *data,
                                    uint32_t samples,
                                    int sample_rate_hz);

/**
 * @brief 打开一轮流式 PCM 播放。
 *
 * 调用方法：server_voice_client 收到本轮第一个 PCM chunk 前调用一次。
 * 本函数会创建 ringbuffer、启动 IIS 和写入任务；同一轮后续 chunk 不会重复初始化。
 */
esp_err_t audio_player_stream_open(void);

/**
 * @brief 向已打开的流式播放器写入一个 PCM chunk。
 *
 * 调用方法：server_voice_client 每收到一个响应 chunk 后调用；必须先 stream_open()。
 *
 * @param data PCM 采样数组。
 * @param samples int16_t 采样点数量，不是字节数。
 * @param sample_rate_hz 输入采样率；当前支持 16 kHz。
 */
esp_err_t audio_player_write_pcm_chunk(const int16_t *data,
                                       uint32_t samples,
                                       int sample_rate_hz);

/**
 * @brief 结束当前流式播放。
 *
 * 调用方法：server_voice_client 读完本轮 PCM 响应后调用；
 * 会等待 ringbuffer 内已入队 PCM 写完，再停止本轮 IIS 输出。
 */
esp_err_t audio_player_stream_finish(void);

/**
 * @brief 播放 1 kHz speaker 自检音。
 *
 * 调用方法：打开 MAIN_ENABLE_SPEAKER_SELF_TEST 后由 app_main 调用；
 * 本函数直接走 speaker_player/IIS，不经过 server voice。
 *
 * @param duration_ms 自检音时长，建议 1000~2000 ms。
 */
esp_err_t audio_player_self_test_1khz(uint32_t duration_ms);

#ifdef __cplusplus
}
#endif

#endif /* SPEAKER_PLAYER_H */
