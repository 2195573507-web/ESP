#ifndef BME_SERVER_CLIENT_H
#define BME_SERVER_CLIENT_H

#include "bme690.h"
#include "bme_air_quality.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* BME690 服务器上传配置：新主链路使用统一设备协议 v1，不再上传旧扁平 /sensor body。 */
#ifndef BME_SERVER_CLIENT_ENDPOINT
#define BME_SERVER_CLIENT_ENDPOINT "/api/device/v1/ingest" // BME690 v1 envelope 上传接口路径。
#endif

#ifndef BME_SERVER_CLIENT_TIMEOUT_MS
#define BME_SERVER_CLIENT_TIMEOUT_MS 5000U // 单次上传超时，单位 ms。
#endif

#ifndef BME_SERVER_CLIENT_JSON_BUFFER_SIZE
#define BME_SERVER_CLIENT_JSON_BUFFER_SIZE 1024U // BME690 v1 JSON body 缓存大小。
#endif

/** 调用方法：BME service 启动时调用一次，当前无状态，可重复调用。 */
esp_err_t bme_server_client_init(void);

/** 调用方法：BME 读数成功后调用；device_id 不能为空，data 传 bme690_read() 输出。 */
esp_err_t bme_server_client_upload_reading(const char *device_id,
                                           const bme690_data_t *data,
                                           const bme_air_quality_result_t *air_quality);

#ifdef __cplusplus
}
#endif

#endif /* BME_SERVER_CLIENT_H */
