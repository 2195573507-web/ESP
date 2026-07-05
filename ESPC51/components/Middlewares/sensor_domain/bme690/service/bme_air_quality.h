#ifndef BME_AIR_QUALITY_H
#define BME_AIR_QUALITY_H

/**
 * @file bme_air_quality.h
 * @brief C5 终端 BME690 空气质量计算接口。
 *
 * BME service 在每次 bme690_read() 成功后调用本模块，输出结果随后由 bme_server_client
 * 放入固定顺序的轻量 v 数组。
 */

#include <stdbool.h>
#include <stdint.h>

#include "bme690.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BME_AIR_QUALITY_ALGO_VERSION "esp-bme690-relative-v1"

typedef struct {
    int air_quality_score;
    const char *air_quality_level;
    const char *air_quality_confidence;
    const char *air_quality_algo_version;
    const char *air_quality_source;
    float gas_baseline_ohm;
    float gas_ratio;
    int gas_score;
    int humidity_score;
    bool baseline_ready;
    bool warmup_done;
    uint32_t sample_count;
} bme_air_quality_result_t;

/** @brief 重置 baseline 和样本计数；调试/重新开始采样时调用。 */
void bme_air_quality_reset(void);
/**
 * @brief 根据一次 BME690 读数更新空气质量估算。
 *
 * 调用位置：bme_sensor_task() 每轮 bme690_read() 成功后。
 * @param data BME690 物理量读数，不能为空。
 * @param out_result 输出空气质量结果，不能为空。
 * @return ESP_OK 表示计算完成；参数为空或 gas 数据无效返回错误码。
 * 失败处理：BME service 记录日志并跳过本轮上传，下个周期重试。
 */
esp_err_t bme_air_quality_update(const bme690_data_t *data,
                                 bme_air_quality_result_t *out_result);

#ifdef __cplusplus
}
#endif

#endif /* BME_AIR_QUALITY_H */
