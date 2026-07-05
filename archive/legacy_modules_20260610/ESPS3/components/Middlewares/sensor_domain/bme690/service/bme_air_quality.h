#ifndef BME_AIR_QUALITY_H
#define BME_AIR_QUALITY_H

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

void bme_air_quality_reset(void);
esp_err_t bme_air_quality_update(const bme690_data_t *data,
                                 bme_air_quality_result_t *out_result);

#ifdef __cplusplus
}
#endif

#endif /* BME_AIR_QUALITY_H */
