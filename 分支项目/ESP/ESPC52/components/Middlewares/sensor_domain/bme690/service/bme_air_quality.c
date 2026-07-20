/**
 * @file bme_air_quality.c
 * @brief C5 终端 BME690 相对空气质量计算。
 *
 * 本文件属于 ESP32-C5 终端（ESPC51/ESPC52 共用），只根据 bme690_read() 的本地读数
 * 计算相对空气质量分数和 baseline。它不读取硬件、不上传数据、不改变统一设备流
 * sensor 帧的 v1/v2/v3 映射。
 */

#include "bme_air_quality.h"

#include <math.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "app_time_sync.h"
#include "nvs.h"
#include "terminal_config.h"

#define BME_AIR_QUALITY_WARMUP_SAMPLE_MIN 30U
#define BME_AIR_QUALITY_BASELINE_MIN_OHM 1000.0f
#define AQ_V3_BASELINE_TRIM_COUNT 3U
#define AQ_V3_STABILITY_WINDOW 8U
#define AQ_V3_BASELINE_ALPHA 0.005f
#define AQ_V3_CONFIDENCE_HIGH 0.75f
#define AQ_V3_BASELINE_MIN_STABILITY_SCORE 0.90f
#define AQ_V3_BASELINE_MIN_GAS_RATIO 0.85f
#define AQ_V3_NVS_NAMESPACE "bme690"
#define AQ_V3_NVS_KEY "baseline_v1"
#define AQ_V3_NVS_VERSION 1U
#define AQ_V3_NVS_MAX_VALID_SAMPLES 100000000U
#define AQ_V3_NVS_MAX_BASELINE_OHM 1000000000.0f
#define AQ_V3_HUMIDITY_REFERENCE 40.0f

static const char *TAG = "bme_air_quality";

typedef struct {
    uint32_t version;
    uint64_t created_time_ms;
    uint64_t update_time_ms;
    float baseline_gas;
    float ema_gas;
    float stability;
    uint32_t valid_samples;
    uint32_t gas_samples;
    float gas_variance;
    float humidity_reference;
    uint8_t baseline_ready;
} bme_air_quality_v3_nvs_t;

static float s_gas_baseline_ohm;
static float s_gas_ema_ohm;
static float s_prev_compensated_gas_ohm;
static float s_score_ema;
static uint32_t s_sample_count;
static bool s_has_prev_compensated_gas;
static bool s_has_score_ema;

static float s_v3_warmup_samples[AQ_V3_BASELINE_VALID_SAMPLES];
static uint32_t s_v3_warmup_sample_count;
static uint32_t s_v3_warmup_write_index;
static uint32_t s_v3_warmup_count;
static uint32_t s_v3_valid_sample_total;
static float s_v3_stability_samples[AQ_V3_STABILITY_WINDOW];
static uint32_t s_v3_stability_sample_count;
static uint32_t s_v3_stability_write_index;
static float s_v3_baseline_ohm;
static float s_v3_prev_compensated_gas_ohm;
static float s_v3_score_ema;
static float s_v3_stability_score;
static float s_v3_gas_ema;
static float s_v3_gas_variance;
static float s_v3_humidity_reference = AQ_V3_HUMIDITY_REFERENCE;
static float s_v3_valid_ratio;
static bool s_v3_baseline_ready;
static bool s_v3_has_previous_gas;
static bool s_v3_has_score_ema;
static bool s_v3_has_gas_ema;
static bool s_v3_has_valid_ratio;
static bool s_v3_nvs_load_attempted;
static uint64_t s_v3_baseline_created_time_ms;
static uint64_t s_v3_baseline_update_time_ms;
static const char *s_v3_last_logged_state;

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
    if (score >= 85) {
        return "excellent";
    }
    if (score >= 70) {
        return "good";
    }
    if (score >= 50) {
        return "moderate";
    }
    if (score >= 30) {
        return "poor";
    }
    return "bad";
}

static float v3_trimmed_mean_baseline(void)
{
    if (s_v3_warmup_sample_count < AQ_V3_BASELINE_VALID_SAMPLES) {
        return 0.0f;
    }

    float sorted[AQ_V3_BASELINE_VALID_SAMPLES];
    for (uint32_t i = 0; i < AQ_V3_BASELINE_VALID_SAMPLES; ++i) {
        sorted[i] = s_v3_warmup_samples[i];
    }
    for (uint32_t i = 1; i < AQ_V3_BASELINE_VALID_SAMPLES; ++i) {
        float value = sorted[i];
        uint32_t j = i;
        while (j > 0U && sorted[j - 1U] > value) {
            sorted[j] = sorted[j - 1U];
            --j;
        }
        sorted[j] = value;
    }

    float sum = 0.0f;
    for (uint32_t i = AQ_V3_BASELINE_TRIM_COUNT;
         i < AQ_V3_BASELINE_VALID_SAMPLES - AQ_V3_BASELINE_TRIM_COUNT;
         ++i) {
        sum += sorted[i];
    }
    return sum / (float)(AQ_V3_BASELINE_VALID_SAMPLES - (2U * AQ_V3_BASELINE_TRIM_COUNT));
}

static void v3_append_warmup_sample(float compensated_gas)
{
    s_v3_warmup_samples[s_v3_warmup_write_index] = compensated_gas;
    s_v3_warmup_write_index =
        (s_v3_warmup_write_index + 1U) % AQ_V3_BASELINE_VALID_SAMPLES;
    if (s_v3_warmup_sample_count < AQ_V3_BASELINE_VALID_SAMPLES) {
        ++s_v3_warmup_sample_count;
    }
}

static float v3_update_stability(float compensated_gas)
{
    float baseline = s_v3_baseline_ready && s_v3_baseline_ohm > 0.0f ?
                         s_v3_baseline_ohm : compensated_gas;
    float delta_ratio = 0.0f;
    if (s_v3_has_previous_gas && baseline > 0.0f) {
        delta_ratio = fabsf(compensated_gas - s_v3_prev_compensated_gas_ohm) / baseline;
    }

    s_v3_stability_samples[s_v3_stability_write_index] = compensated_gas;
    s_v3_stability_write_index =
        (s_v3_stability_write_index + 1U) % AQ_V3_STABILITY_WINDOW;
    if (s_v3_stability_sample_count < AQ_V3_STABILITY_WINDOW) {
        ++s_v3_stability_sample_count;
    }

    float mean = 0.0f;
    for (uint32_t i = 0; i < s_v3_stability_sample_count; ++i) {
        mean += s_v3_stability_samples[i];
    }
    mean /= (float)s_v3_stability_sample_count;

    float variance = 0.0f;
    for (uint32_t i = 0; i < s_v3_stability_sample_count; ++i) {
        float deviation = s_v3_stability_samples[i] - mean;
        variance += deviation * deviation;
    }
    variance /= (float)s_v3_stability_sample_count;
    float coefficient_of_variation = mean > 0.0f ? sqrtf(variance) / mean : 1.0f;

    s_v3_prev_compensated_gas_ohm = compensated_gas;
    s_v3_has_previous_gas = true;
    s_v3_stability_score =
        clamp_float(1.0f - (delta_ratio * 5.0f) - (coefficient_of_variation * 5.0f), 0.0f, 1.0f);
    s_v3_gas_variance = variance;
    return delta_ratio;
}

static float v3_update_valid_ratio(bool sample_valid)
{
    float sample_value = sample_valid ? 1.0f : 0.0f;
    if (!s_v3_has_valid_ratio) {
        s_v3_valid_ratio = sample_value;
        s_v3_has_valid_ratio = true;
    } else {
        s_v3_valid_ratio = s_v3_valid_ratio * 0.90f + sample_value * 0.10f;
    }
    return s_v3_valid_ratio;
}

static const char *v3_confidence_label(float confidence)
{
    if (confidence >= AQ_V3_CONFIDENCE_HIGH) {
        return "high";
    }
    if (confidence >= 0.45f) {
        return "medium";
    }
    return "low";
}

static void v3_log_baseline_state(const char *state)
{
    if (s_v3_last_logged_state != NULL && strcmp(state, s_v3_last_logged_state) == 0) {
        return;
    }
    const char *device_id = terminal_config_get_device_id();
    ESP_LOGI(TAG,
             "AQ_V3_BASELINE_STATE device=%s state=%s valid_count=%lu baseline_ready=%d baseline=%.0f",
             device_id != NULL ? device_id : "unknown",
             state,
             (unsigned long)s_v3_warmup_count,
             s_v3_baseline_ready ? 1 : 0,
             (double)s_v3_baseline_ohm);
    s_v3_last_logged_state = state;
}

static void bme_air_quality_v3_reset(void)
{
    memset(s_v3_warmup_samples, 0, sizeof(s_v3_warmup_samples));
    memset(s_v3_stability_samples, 0, sizeof(s_v3_stability_samples));
    s_v3_warmup_sample_count = 0;
    s_v3_warmup_write_index = 0;
    s_v3_warmup_count = 0;
    s_v3_valid_sample_total = 0;
    s_v3_stability_sample_count = 0;
    s_v3_stability_write_index = 0;
    s_v3_baseline_ohm = 0.0f;
    s_v3_prev_compensated_gas_ohm = 0.0f;
    s_v3_score_ema = 0.0f;
    s_v3_stability_score = 0.0f;
    s_v3_gas_ema = 0.0f;
    s_v3_gas_variance = 0.0f;
    s_v3_humidity_reference = AQ_V3_HUMIDITY_REFERENCE;
    s_v3_valid_ratio = 0.0f;
    s_v3_baseline_ready = false;
    s_v3_has_previous_gas = false;
    s_v3_has_score_ema = false;
    s_v3_has_gas_ema = false;
    s_v3_has_valid_ratio = false;
    s_v3_baseline_created_time_ms = 0U;
    s_v3_baseline_update_time_ms = 0U;
    s_v3_last_logged_state = NULL;
}

static uint64_t v3_current_unix_ms(void)
{
    if (!app_time_sync_is_synced()) {
        return 0U;
    }

    int64_t unix_ms = app_time_sync_get_unix_ms();
    return unix_ms > 0 ? (uint64_t)unix_ms : 0U;
}

static bool v3_persisted_baseline_is_valid(const bme_air_quality_v3_nvs_t *persisted)
{
    if (persisted == NULL || persisted->version != AQ_V3_NVS_VERSION ||
        persisted->baseline_ready != 1U ||
        !isfinite(persisted->baseline_gas) ||
        persisted->baseline_gas < BME_AIR_QUALITY_BASELINE_MIN_OHM ||
        persisted->baseline_gas > AQ_V3_NVS_MAX_BASELINE_OHM ||
        !isfinite(persisted->ema_gas) || persisted->ema_gas <= 0.0f ||
        !isfinite(persisted->stability) || persisted->stability < 0.0f ||
        persisted->stability > 1.0f ||
        persisted->valid_samples < AQ_V3_BASELINE_VALID_SAMPLES ||
        persisted->valid_samples > AQ_V3_NVS_MAX_VALID_SAMPLES ||
        persisted->gas_samples < AQ_V3_BASELINE_VALID_SAMPLES ||
        persisted->gas_samples > AQ_V3_NVS_MAX_VALID_SAMPLES ||
        !isfinite(persisted->gas_variance) || persisted->gas_variance < 0.0f ||
        persisted->gas_variance > persisted->baseline_gas * persisted->baseline_gas * 4.0f ||
        !isfinite(persisted->humidity_reference) ||
        persisted->humidity_reference < 0.0f || persisted->humidity_reference > 100.0f) {
        return false;
    }
    return true;
}

static void v3_erase_persisted_baseline(void)
{
    nvs_handle_t nvs = 0;
    esp_err_t ret = nvs_open(AQ_V3_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "AQ_V3 baseline NVS erase open failed: %s", esp_err_to_name(ret));
        return;
    }

    ret = nvs_erase_key(nvs, AQ_V3_NVS_KEY);
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs);
    }
    if (ret != ESP_OK && ret != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "AQ_V3 baseline NVS erase failed: %s", esp_err_to_name(ret));
    }
    nvs_close(nvs);
}

static void v3_persist_baseline(void)
{
    if (!s_v3_baseline_ready || !s_v3_has_gas_ema) {
        return;
    }

    uint64_t now_ms = v3_current_unix_ms();
    if (s_v3_baseline_created_time_ms == 0U && now_ms != 0U) {
        s_v3_baseline_created_time_ms = now_ms;
    }
    s_v3_baseline_update_time_ms = now_ms;

    bme_air_quality_v3_nvs_t persisted = {
        .version = AQ_V3_NVS_VERSION,
        .created_time_ms = s_v3_baseline_created_time_ms,
        .update_time_ms = s_v3_baseline_update_time_ms,
        .baseline_gas = s_v3_baseline_ohm,
        .ema_gas = s_v3_gas_ema,
        .stability = s_v3_stability_score,
        .valid_samples = s_v3_valid_sample_total,
        .gas_samples = s_v3_valid_sample_total,
        .gas_variance = s_v3_gas_variance,
        .humidity_reference = s_v3_humidity_reference,
        .baseline_ready = 1U,
    };

    nvs_handle_t nvs = 0;
    esp_err_t ret = nvs_open(AQ_V3_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "AQ_V3 baseline NVS open failed: %s", esp_err_to_name(ret));
        return;
    }
    ret = nvs_set_blob(nvs, AQ_V3_NVS_KEY, &persisted, sizeof(persisted));
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs);
    }
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "AQ_V3 baseline NVS save failed: %s", esp_err_to_name(ret));
    }
    nvs_close(nvs);
}

static void v3_restore_baseline(void)
{
    if (s_v3_nvs_load_attempted) {
        return;
    }

    nvs_handle_t nvs = 0;
    esp_err_t ret = nvs_open(AQ_V3_NVS_NAMESPACE, NVS_READWRITE, &nvs);
    if (ret == ESP_ERR_NVS_NOT_INITIALIZED) {
        ESP_LOGW(TAG, "AQ_V3 baseline NVS not initialized; restore will retry");
        return;
    }
    s_v3_nvs_load_attempted = true;
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "AQ_V3 baseline NVS open failed: %s", esp_err_to_name(ret));
        return;
    }

    bme_air_quality_v3_nvs_t persisted = {0};
    size_t persisted_size = sizeof(persisted);
    ret = nvs_get_blob(nvs, AQ_V3_NVS_KEY, &persisted, &persisted_size);
    nvs_close(nvs);
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "AQ_V3 baseline not found; starting normal learning");
        return;
    }
    if (ret != ESP_OK || persisted_size != sizeof(persisted) ||
        !v3_persisted_baseline_is_valid(&persisted)) {
        ESP_LOGW(TAG, "AQ_V3 baseline invalid; discarding and relearning");
        bme_air_quality_v3_reset();
        v3_erase_persisted_baseline();
        return;
    }

    bme_air_quality_v3_reset();
    s_v3_baseline_ohm = persisted.baseline_gas;
    s_v3_gas_ema = persisted.ema_gas;
    s_v3_stability_score = persisted.stability;
    s_v3_gas_variance = persisted.gas_variance;
    s_v3_humidity_reference = persisted.humidity_reference;
    s_v3_valid_sample_total = persisted.valid_samples;
    s_v3_baseline_ready = true;
    s_v3_has_gas_ema = true;
    s_v3_baseline_created_time_ms = persisted.created_time_ms;
    s_v3_baseline_update_time_ms = persisted.update_time_ms;
    ESP_LOGI(TAG,
             "AQ_V3 baseline restored gas=%.0f samples=%lu created=%llu updated=%llu",
             (double)s_v3_baseline_ohm,
             (unsigned long)s_v3_valid_sample_total,
             (unsigned long long)s_v3_baseline_created_time_ms,
             (unsigned long long)s_v3_baseline_update_time_ms);
}

void bme_air_quality_init(void)
{
    v3_restore_baseline();
}

void bme_air_quality_reset(void)
{
    s_gas_baseline_ohm = 0.0f;
    s_gas_ema_ohm = 0.0f;
    s_prev_compensated_gas_ohm = 0.0f;
    s_score_ema = 0.0f;
    s_sample_count = 0;
    s_has_prev_compensated_gas = false;
    s_has_score_ema = false;
    bme_air_quality_v3_reset();
    s_v3_nvs_load_attempted = true;
    v3_erase_persisted_baseline();
}

esp_err_t bme_air_quality_update_v2(const bme690_data_t *data,
                                    bme_air_quality_result_t *out_result)
{
    if (data == NULL || out_result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out_result, 0, sizeof(*out_result));
    out_result->air_quality_algo_version = BME_AIR_QUALITY_V2_ALGO_VERSION;
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
    if (s_gas_ema_ohm <= 0.0f || !isfinite(s_gas_ema_ohm)) {
        s_gas_ema_ohm = gas_ohm;
    } else {
        s_gas_ema_ohm = s_gas_ema_ohm * 0.85f + gas_ohm * 0.15f;
    }

    float humidity_factor = 1.0f;
    bool humidity_valid = isfinite(humidity) && humidity >= 0.0f && humidity <= 100.0f;
    if (humidity_valid) {
        humidity_factor += clamp_float((humidity - 40.0f) * 0.006f, -0.12f, 0.18f);
    }
    float compensated_gas = s_gas_ema_ohm / humidity_factor;
    if (!isfinite(compensated_gas) || compensated_gas <= 0.0f) {
        return ESP_ERR_INVALID_STATE;
    }

    if (s_gas_baseline_ohm < BME_AIR_QUALITY_BASELINE_MIN_OHM) {
        s_gas_baseline_ohm = compensated_gas > BME_AIR_QUALITY_BASELINE_MIN_OHM ?
                             compensated_gas : BME_AIR_QUALITY_BASELINE_MIN_OHM;
    } else if (compensated_gas > s_gas_baseline_ohm) {
        s_gas_baseline_ohm = s_gas_baseline_ohm * 0.995f + compensated_gas * 0.005f;
    } else {
        s_gas_baseline_ohm = s_gas_baseline_ohm * 0.9995f + compensated_gas * 0.0005f;
        if (s_gas_baseline_ohm < BME_AIR_QUALITY_BASELINE_MIN_OHM) {
            s_gas_baseline_ohm = BME_AIR_QUALITY_BASELINE_MIN_OHM;
        }
    }

    float gas_ratio = clamp_float(compensated_gas / s_gas_baseline_ohm, 0.0f, 1.5f);
    float drop_ratio =
        clamp_float((s_gas_baseline_ohm - compensated_gas) / s_gas_baseline_ohm, 0.0f, 0.75f);
    float gas_penalty = 100.0f * (1.0f - expf(-4.0f * drop_ratio));
    float gas_score_float = clamp_float(100.0f - gas_penalty, 0.0f, 100.0f);
    int gas_score = clamp_int((int)lroundf(gas_score_float), 0, 100);

    float volatility_penalty = 0.0f;
    if (s_has_prev_compensated_gas) {
        float delta_ratio = fabsf(compensated_gas - s_prev_compensated_gas_ohm) /
                            s_gas_baseline_ohm;
        volatility_penalty = clamp_float(delta_ratio * 120.0f, 0.0f, 20.0f);
    }
    s_prev_compensated_gas_ohm = compensated_gas;
    s_has_prev_compensated_gas = true;

    float humidity_penalty = 20.0f;
    if (humidity_valid) {
        if (humidity >= 40.0f && humidity <= 60.0f) {
            humidity_penalty = 0.0f;
        } else if (humidity < 40.0f) {
            humidity_penalty = clamp_float((40.0f - humidity) / 40.0f * 20.0f, 0.0f, 20.0f);
        } else {
            humidity_penalty = clamp_float((humidity - 60.0f) / 40.0f * 20.0f, 0.0f, 20.0f);
        }
    }
    int humidity_score = clamp_int((int)lroundf(100.0f - humidity_penalty), 0, 100);

    float score = clamp_float(gas_score_float -
                              volatility_penalty -
                              humidity_penalty * 0.35f,
                              0.0f,
                              100.0f);
    if (!s_has_score_ema) {
        s_score_ema = score;
        s_has_score_ema = true;
    } else {
        s_score_ema = s_score_ema * 0.7f + score * 0.3f;
    }
    int final_score = clamp_int((int)lroundf(s_score_ema), 0, 100);
    bool warmup_done = s_sample_count >= BME_AIR_QUALITY_WARMUP_SAMPLE_MIN;
    bool temperature_valid = isfinite(temperature) &&
                             temperature >= -10.0f &&
                             temperature <= 60.0f;

    out_result->air_quality_score = final_score;
    out_result->air_quality_level = level_for_score(final_score);
    out_result->air_quality_confidence =
        warmup_done && humidity_valid && temperature_valid ? "high" : "low";
    out_result->gas_baseline_ohm = s_gas_baseline_ohm;
    out_result->gas_ratio = gas_ratio;
    out_result->gas_score = gas_score;
    out_result->humidity_score = humidity_score;
    out_result->baseline_ready = warmup_done;
    out_result->warmup_done = warmup_done;
    out_result->sample_count = s_sample_count;
    ESP_LOGI(TAG,
             "BME_AQ_V2 score=%d level=%s gas=%.0f baseline=%.0f ratio=%.3f samples=%lu",
             final_score,
             out_result->air_quality_level,
             (double)gas_ohm,
             (double)s_gas_baseline_ohm,
             (double)gas_ratio,
             (unsigned long)s_sample_count);
    return ESP_OK;
}

static esp_err_t bme_air_quality_update_v3(const bme690_data_t *data,
                                            bme_air_quality_result_t *out_result)
{
    if (data == NULL || out_result == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out_result, 0, sizeof(*out_result));
    out_result->air_quality_algo_version = BME_AIR_QUALITY_V3_ALGO_VERSION;
    out_result->air_quality_source = "esp";

    bme_air_quality_init();

    float gas_ohm = (float)data->gas_resistance_ohm;
    float humidity = data->humidity_percent;
    bool humidity_valid = isfinite(humidity) && humidity >= 0.0f && humidity <= 100.0f;
    bool sample_valid = data->gas_valid && data->heat_stable && isfinite(gas_ohm) && gas_ohm > 0.0f;
    float compensated_gas = 0.0f;
    if (sample_valid) {
        float humidity_factor = humidity_valid ?
                                    1.0f + clamp_float((humidity - 40.0f) * 0.006f, -0.12f, 0.18f) :
                                    1.0f;
        compensated_gas = gas_ohm / humidity_factor;
        sample_valid = isfinite(compensated_gas) && compensated_gas > 0.0f;
    }

    float valid_ratio = v3_update_valid_ratio(sample_valid);
    float delta_ratio = 0.0f;
    bool stability_updated = false;

    if (!sample_valid) {
        if (!s_v3_baseline_ready && s_v3_warmup_count > 0U) {
            --s_v3_warmup_count;
        }
    } else {
        ++s_v3_valid_sample_total;
        if (!s_v3_has_gas_ema) {
            s_v3_gas_ema = compensated_gas;
            s_v3_has_gas_ema = true;
        } else {
            s_v3_gas_ema = s_v3_gas_ema * 0.90f + compensated_gas * 0.10f;
        }
    }

    if (sample_valid && !s_v3_baseline_ready) {
        v3_append_warmup_sample(compensated_gas);
        if (s_v3_warmup_count < AQ_V3_BASELINE_VALID_SAMPLES) {
            ++s_v3_warmup_count;
        }
        delta_ratio = v3_update_stability(compensated_gas);
        stability_updated = true;
        if (s_v3_warmup_count >= AQ_V3_BASELINE_VALID_SAMPLES &&
            s_v3_warmup_sample_count >= AQ_V3_BASELINE_VALID_SAMPLES) {
            s_v3_baseline_ohm = v3_trimmed_mean_baseline();
            s_v3_baseline_ready = isfinite(s_v3_baseline_ohm) &&
                                  s_v3_baseline_ohm > 0.0f;
            if (s_v3_baseline_ready) {
                v3_persist_baseline();
            }
        }
    }

    if (sample_valid && !stability_updated) {
        delta_ratio = v3_update_stability(compensated_gas);
    }

    const char *sensor_state = !s_v3_baseline_ready ? "WARMUP" :
                               sample_valid ? "READY" : "DEGRADED";
    float gas_ratio = s_v3_baseline_ready && s_v3_baseline_ohm > 0.0f && sample_valid ?
                          clamp_float(compensated_gas / s_v3_baseline_ohm, 0.0f, 1.5f) :
                          0.0f;
    int final_score = s_v3_has_score_ema ? clamp_int((int)lroundf(s_v3_score_ema), 0, 100) : 50;
    int gas_score = final_score;
    int humidity_score = humidity_valid ?
                             clamp_int((int)lroundf(100.0f -
                                                    clamp_float(fabsf(humidity - 50.0f) / 40.0f * 20.0f,
                                                                0.0f,
                                                                20.0f)),
                                       0,
                                       100) :
                             80;
    float stability_score = sample_valid ? s_v3_stability_score : s_v3_stability_score * 0.5f;
    float trend_score = clamp_float(1.0f - (delta_ratio * 5.0f), 0.0f, 1.0f);
    float confidence = clamp_float((s_v3_baseline_ready ? 0.45f : 0.0f) +
                                       valid_ratio * 0.25f +
                                       stability_score * 0.20f +
                                       trend_score * 0.10f,
                                   0.0f,
                                   1.0f);

    if (sample_valid && s_v3_baseline_ready) {
        stability_score = s_v3_stability_score;
        trend_score = clamp_float(1.0f - (delta_ratio * 5.0f), 0.0f, 1.0f);
        confidence = clamp_float(0.45f + valid_ratio * 0.25f + stability_score * 0.20f + trend_score * 0.10f,
                                 0.0f,
                                 1.0f);
        float gas_score_float = gas_ratio >= 1.0f ? 100.0f :
                                clamp_float(powf(gas_ratio, 1.75f) * 100.0f, 0.0f, 100.0f);
        gas_score = clamp_int((int)lroundf(gas_score_float), 0, 100);
        float humidity_penalty = humidity_valid ?
                                     clamp_float(fabsf(humidity - 50.0f) / 40.0f * 20.0f, 0.0f, 20.0f) :
                                     20.0f;
        float score = clamp_float(gas_score_float - humidity_penalty * 0.35f -
                                      (1.0f - stability_score) * 15.0f,
                                  0.0f,
                                  100.0f);
        if (!s_v3_has_score_ema) {
            s_v3_score_ema = score;
            s_v3_has_score_ema = true;
        } else {
            s_v3_score_ema = s_v3_score_ema * 0.70f + score * 0.30f;
        }
        final_score = clamp_int((int)lroundf(s_v3_score_ema), 0, 100);
        const char *level = level_for_score(final_score);
        if ((strcmp(level, "excellent") == 0 || strcmp(level, "good") == 0) &&
            confidence >= AQ_V3_CONFIDENCE_HIGH &&
            stability_score >= AQ_V3_BASELINE_MIN_STABILITY_SCORE &&
            gas_ratio >= AQ_V3_BASELINE_MIN_GAS_RATIO &&
            s_v3_stability_sample_count >= AQ_V3_STABILITY_WINDOW) {
            s_v3_baseline_ohm = s_v3_baseline_ohm * (1.0f - AQ_V3_BASELINE_ALPHA) +
                                compensated_gas * AQ_V3_BASELINE_ALPHA;
            gas_ratio = clamp_float(compensated_gas / s_v3_baseline_ohm, 0.0f, 1.5f);
            v3_persist_baseline();
        }
    } else if (!sample_valid) {
        confidence *= 0.35f;
    }

    const char *level = s_v3_baseline_ready ? level_for_score(final_score) : "unknown";
    const char *confidence_label = v3_confidence_label(confidence);
    out_result->air_quality_score = final_score;
    out_result->air_quality_level = level;
    out_result->air_quality_confidence = confidence_label;
    out_result->gas_baseline_ohm = s_v3_baseline_ohm;
    out_result->gas_ratio = gas_ratio;
    out_result->gas_score = gas_score;
    out_result->humidity_score = humidity_score;
    out_result->baseline_ready = s_v3_baseline_ready;
    out_result->warmup_done = s_v3_baseline_ready;
    out_result->sample_count = s_v3_valid_sample_total;
    out_result->air_quality.algorithm = BME_AIR_QUALITY_V3_ALGO_VERSION;
    out_result->air_quality.score = final_score;
    out_result->air_quality.level = level;
    out_result->air_quality.confidence = confidence;
    out_result->air_quality.gas_ratio = gas_ratio;
    out_result->air_quality.stability_score = stability_score;
    out_result->air_quality.sensor_state = sensor_state;
    out_result->air_quality.baseline_ready = s_v3_baseline_ready;
    out_result->air_quality.baseline_created_time_ms = s_v3_baseline_created_time_ms;
    out_result->air_quality.baseline_update_time_ms = s_v3_baseline_update_time_ms;
    if (s_v3_baseline_ready && s_v3_baseline_created_time_ms == 0U &&
        v3_current_unix_ms() != 0U) {
        v3_persist_baseline();
        out_result->air_quality.baseline_created_time_ms = s_v3_baseline_created_time_ms;
        out_result->air_quality.baseline_update_time_ms = s_v3_baseline_update_time_ms;
    }
    v3_log_baseline_state(sensor_state);
    return ESP_OK;
}

esp_err_t bme_air_quality_update(const bme690_data_t *data,
                                 bme_air_quality_result_t *out_result)
{
    return bme_air_quality_update_v3(data, out_result);
}
