#ifndef BME_CACHE_MANAGER_H
#define BME_CACHE_MANAGER_H

/**
 * @file bme_cache_manager.h
 * @brief ESPS3 BME690 RAM ring buffer for reliable gateway upload.
 *
 * The cache stores C5-produced BME readings and the exact Server ingest JSON.
 * ESPS3 only validates and transports these fields; it does not recalculate
 * C5 air-quality outputs.
 */

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BME_CACHE_MANAGER_CAPACITY 100U
#define BME_CACHE_MANAGER_TEXT_LEN 64U
#define BME_CACHE_MANAGER_LEVEL_LEN 24U
#define BME_CACHE_MANAGER_CONFIDENCE_LEN 24U

typedef struct {
    uint32_t sequence;
    char device_id[BME_CACHE_MANAGER_TEXT_LEN];
    int64_t timestamp_ms;
    double temperature_c;
    double humidity_percent;
    double pressure_hpa;
    double gas_resistance_ohm;
    double air_quality_score;
    double gas_baseline_ohm;
    double gas_ratio;
    double gas_score;
    double humidity_score;
    uint32_t sample_count;
    char level[BME_CACHE_MANAGER_LEVEL_LEN];
    char confidence[BME_CACHE_MANAGER_CONFIDENCE_LEN];
    char algorithm_version[BME_CACHE_MANAGER_TEXT_LEN];
    bool in_flight;
    char *server_json;
} bme_cache_record_t;

esp_err_t bme_cache_manager_init(void);
esp_err_t bme_cache_manager_push_json(const char *server_json,
                                      uint32_t *out_sequence,
                                      size_t *out_size);
esp_err_t bme_cache_manager_peek_oldest(bme_cache_record_t *out_record);
/** @brief 拷贝指定设备最老且未 in-flight 的记录；不删除缓存。 */
esp_err_t bme_cache_manager_peek_oldest_for_device(const char *device_id,
                                                   bme_cache_record_t *out_record);
esp_err_t bme_cache_manager_delete_oldest(uint32_t sequence);
esp_err_t bme_cache_manager_delete_sequence(uint32_t sequence);
esp_err_t bme_cache_manager_mark_in_flight(uint32_t sequence, bool in_flight);
void bme_cache_manager_release_record(bme_cache_record_t *record);
size_t bme_cache_manager_size(void);

#ifdef __cplusplus
}
#endif

#endif /* BME_CACHE_MANAGER_H */
