/**
 * @file bme_air_quality.c
 * @brief C5 终端 BME690 相对空气质量计算。
 *
 * 本文件属于 ESP32-C5 终端（ESPC51/ESPC52 共用），只根据 bme690_read() 的本地读数
 * 计算相对空气质量分数和 baseline。它不读取硬件、不上传数据、不改变 /local/v1/sensor
 * 的字段顺序。
 */

#include "bme_air_quality.h"

#include <math.h>
#include <string.h>

#include "esp_err.h"

#define BME_AIR_QUALITY_WARMUP_SAMPLE_MIN 30U
#define BME_AIR_QUALITY_BASELINE_MIN_OHM 1000.0f

static float s_gas_baseline_ohm;
static uint32_t s_sample_count;

static float clamp_float(float value, float min_value, float max_value)
{
    if (value < min_value) {
        return min_value;
    }
    if (value > max_value) {
        return max_value;
    }
    return value;
}

static int clamp_int(int value, int min_value, int max_value)
{
    if (value < min_value) {
        return min_value;
    }
    if (value > max_value) {
        return max_value;
    }
    return value;
}

static const char *level_for_score(int score)
{
    if (score >= 90) {
        return "excellent";
    }
    if (score >= 75) {
        return "good";
    }
    if (score >= 55) {
        return "moderate";
    }
    if (score >= 30) {
        return "poor";
    }
    return "bad";
}

void bme_air_quality_reset(void)
{
    s_gas_baseline_ohm = 0.0f;
    s_sample_count = 0;
}

esp_err_t bme_air_quality_update(const bme690_data_t *data,
                                 bme_air_quality_result_t *out_result)
{
    if (data == NULL || out_result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out_result, 0, sizeof(*out_result));
    out_result->air_quality_algo_version = BME_AIR_QUALITY_ALGO_VERSION;
    out_result->air_quality_source = "esp";
    out_result->air_quality_level = "unknown";
    out_result->air_quality_confidence = "none";

    float gas_ohm = (float)data->gas_resistance_ohm;
    float humidity = data->humidity_percent;
    float temperature = data->temperature_c;
    if (!isfinite(gas_ohm) || gas_ohm <= 0.0f) {
        return ESP_ERR_INVALID_STATE;
    }

    s_sample_count++;
    if (s_gas_baseline_ohm < BME_AIR_QUALITY_BASELINE_MIN_OHM) {
        s_gas_baseline_ohm = gas_ohm > BME_AIR_QUALITY_BASELINE_MIN_OHM ?
                             gas_ohm : BME_AIR_QUALITY_BASELINE_MIN_OHM;
    } else if (gas_ohm > s_gas_baseline_ohm) {
        s_gas_baseline_ohm = s_gas_baseline_ohm * 0.99f + gas_ohm * 0.01f;
    } else {
        s_gas_baseline_ohm = s_gas_baseline_ohm * 0.999f + gas_ohm * 0.001f;
        if (s_gas_baseline_ohm < BME_AIR_QUALITY_BASELINE_MIN_OHM) {
            s_gas_baseline_ohm = BME_AIR_QUALITY_BASELINE_MIN_OHM;
        }
    }

    float gas_ratio = clamp_float(gas_ohm / s_gas_baseline_ohm, 0.0f, 1.5f);
    int gas_score = clamp_int((int)lroundf(gas_ratio * 100.0f), 0, 100);
    int humidity_score = 50;
    bool humidity_valid = isfinite(humidity) && humidity >= 0.0f && humidity <= 100.0f;
    if (humidity_valid) {
        float humidity_deviation = fabsf(humidity - 50.0f);
        humidity_score = clamp_int((int)lroundf(100.0f - humidity_deviation * 2.5f), 0, 100);
    }

    int score = clamp_int((int)lroundf((float)gas_score * 0.75f +
                                      (float)humidity_score * 0.25f),
                          0,
                          100);
    bool warmup_done = s_sample_count >= BME_AIR_QUALITY_WARMUP_SAMPLE_MIN;
    bool temperature_valid = isfinite(temperature) &&
                             temperature >= -10.0f &&
                             temperature <= 60.0f;

    out_result->air_quality_score = score;
    out_result->air_quality_level = level_for_score(score);
    out_result->air_quality_confidence =
        warmup_done && humidity_valid && temperature_valid ? "medium" : "low";
    out_result->gas_baseline_ohm = s_gas_baseline_ohm;
    out_result->gas_ratio = gas_ratio;
    out_result->gas_score = gas_score;
    out_result->humidity_score = humidity_score;
    out_result->baseline_ready = warmup_done;
    out_result->warmup_done = warmup_done;
    out_result->sample_count = s_sample_count;
    return ESP_OK;
}
