#ifndef BSP_IIS_H
#define BSP_IIS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/i2s_pdm.h"
#include "driver/i2s_types.h"
#include "esp_err.h"

/**
 * @file iis.h
 * @brief ESP32-C5 IIS/I2S PDM 扬声器 BSP。
 *
 * 硬件输出格式：
 *   PCM16 单声道 16 kHz -> I2S PDM TX 硬件 PCM2PDM。
 *
 * 职责边界：
 * 1. 本 BSP 统一持有 I2S/PDM TX channel、GPIO 时序参数和 PA 控制脚。
 * 2. speaker 上层只调用 iis_* 接口，不直接调用 ESP-IDF I2S driver。
 * 3. iis_init() 只完成底层准备，真正输出由 iis_start()/iis_write()/iis_stop() 控制。
 */

#ifndef IIS_PDM_GPIO_CLK
/* PDM 时钟输出 GPIO，连接到功放/Codec 的 PDM CLK。 */
#define IIS_PDM_GPIO_CLK GPIO_NUM_8
#endif

#ifndef IIS_PDM_GPIO_DATA
/* PDM 主数据输出 GPIO，当前硬件只使用一路数据线。 */
#define IIS_PDM_GPIO_DATA GPIO_NUM_7
#endif

#ifndef IIS_PDM_GPIO_DATA2
/* 第二路 PDM 数据线；不用时保持 GPIO_NUM_NC。 */
#define IIS_PDM_GPIO_DATA2 GPIO_NUM_NC
#endif

#ifndef IIS_PDM_CLK_INVERT
/* PDM CLK 是否反相；硬件相位异常时才需要改为 1。 */
#define IIS_PDM_CLK_INVERT 0
#endif

#ifndef IIS_PDM_UPSAMPLE_FP
/* PCM2PDM 升采样分子，配合 FS 形成 ESP-IDF PDM 时钟比例。 */
#define IIS_PDM_UPSAMPLE_FP 960
#endif

#ifndef IIS_PDM_UPSAMPLE_FS
/* PCM2PDM 升采样分母；16 kHz 下为 160，对应约 6.144 MHz PDM clock。 */
#define IIS_PDM_UPSAMPLE_FS (IIS_SAMPLE_RATE_HZ / 100)
#endif

#ifndef IIS_GPIO_PA_CTL
/* 功放使能 GPIO；若硬件没有 PA 控制脚，可定义为 GPIO_NUM_NC。 */
#define IIS_GPIO_PA_CTL GPIO_NUM_1
#endif

#ifndef IIS_PA_ENABLE_LEVEL
/* 功放使能有效电平，默认高电平打开功放。 */
#define IIS_PA_ENABLE_LEVEL 1
#endif

#ifndef IIS_PA_ENABLE_SETTLE_MS
/* 功放使能后等待模拟侧稳定的时间。 */
#define IIS_PA_ENABLE_SETTLE_MS 20
#endif

#ifndef IIS_PORT
/* I2S/PDM 控制器编号。 */
#define IIS_PORT I2S_NUM_0
#endif

#ifndef IIS_SAMPLE_RATE_HZ
/* speaker 层公开输入采样率：PCM16 mono 16 kHz，和 ESP-server audio/L16 保持一致。 */
#define IIS_SAMPLE_RATE_HZ 16000
#endif

#ifndef IIS_BITS_PER_SAMPLE
/* 每个 PCM 采样点的位宽，当前固定 16 bit。 */
#define IIS_BITS_PER_SAMPLE I2S_DATA_BIT_WIDTH_16BIT
#endif

#ifndef IIS_PDM_SLOT_MODE
/* PDM 输出声道模式；当前 speaker 只走单声道。 */
#define IIS_PDM_SLOT_MODE I2S_SLOT_MODE_MONO
#endif

#ifndef IIS_PDM_SLOT_MASK
/* 单声道输出使用的 slot。 */
#define IIS_PDM_SLOT_MASK I2S_PDM_SLOT_LEFT
#endif

#ifndef IIS_DMA_DESC_NUM
/* DMA 描述符数量，过小会在 BSP 内抬到安全下限。 */
#define IIS_DMA_DESC_NUM 8
#endif

#ifndef IIS_DMA_FRAME_NUM
/* 单个 DMA 描述符承载的 frame 数，过小会在 BSP 内抬到安全下限。 */
#define IIS_DMA_FRAME_NUM 512
#endif

/* 实际传给 I2S driver 的 DMA 参数下限，避免播放中 DMA 过快饥饿。 */
#define IIS_EFFECTIVE_DMA_DESC_NUM \
    ((IIS_DMA_DESC_NUM) < 8 ? 8 : (IIS_DMA_DESC_NUM))

#define IIS_EFFECTIVE_DMA_FRAME_NUM \
    ((IIS_DMA_FRAME_NUM) < 512 ? 512 : (IIS_DMA_FRAME_NUM))

/* 当前 PCM2PDM 参数期望生成约 6.144 MHz PDM clock，低于 95% 视为配置异常。 */
#define IIS_EXPECTED_PDM_CLOCK_HZ 6144000UL
#define IIS_PDM_CLOCK_LOW_LIMIT_HZ ((IIS_EXPECTED_PDM_CLOCK_HZ * 95UL) / 100UL)

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 I2S PDM TX channel 和 PA 控制脚。
 *
 * 调用方法：speaker 初始化阶段调用一次即可；函数可重复调用，已初始化时直接返回。
 * 初始化完成后 channel 仍处于停止状态，需要播放时再调用 iis_start()。
 */
esp_err_t iis_init(void);

/**
 * @brief 启动 I2S PDM TX 输出。
 *
 * 调用方法：写入 PCM 前调用；内部会先确保 iis_init() 已完成。
 */
esp_err_t iis_start(void);

/**
 * @brief 向 I2S/PDM TX 写入 PCM 字节流。
 *
 * 调用方法：speaker 的写入任务独占调用；其他模块不要绕过 speaker 直接写 IIS。
 *
 * @param data PCM16 mono 数据指针。
 * @param bytes 待写入字节数，必须大于 0。
 * @param bytes_written 实际写入字节数，可为 NULL。
 * @param timeout_ms I2S driver 写入超时时间，0 表示非阻塞轮询。
 */
esp_err_t iis_write(const void *data,
                    size_t bytes,
                    size_t *bytes_written,
                    uint32_t timeout_ms);

/**
 * @brief 停止 I2S PDM TX 输出。
 *
 * 调用方法：一段 PCM 播放结束后调用；重复停止会直接返回 ESP_OK。
 */
esp_err_t iis_stop(void);

/**
 * @brief 释放 I2S PDM TX channel 和本轮播放占用的 driver 资源。
 *
 * 调用方法：speaker 播放结束或播放初始化失败后调用；下次播放会由 iis_start()
 * 重新 lazy init。释放 channel 可以避免 PCM 播放结束后继续占用连续 8-bit heap。
 */
esp_err_t iis_deinit(void);

/**
 * @brief 读取 I2S channel 当前信息，用于启动前后的 DMA/clock 诊断。
 *
 * 调用方法：播放链路调试时调用；out_info 不能为空。
 */
esp_err_t iis_get_info(i2s_chan_info_t *out_info);

/**
 * @brief 查询底层 I2S/PDM TX 是否已 enable。
 *
 * 调用方法：speaker 或诊断日志判断当前是否正在占用 IIS。
 */
bool iis_is_started(void);

#ifdef __cplusplus
}
#endif

#endif /* BSP_IIS_H */
