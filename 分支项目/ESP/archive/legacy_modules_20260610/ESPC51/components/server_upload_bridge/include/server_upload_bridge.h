#ifndef SERVER_UPLOAD_BRIDGE_H
#define SERVER_UPLOAD_BRIDGE_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* BME690 上传桥配置：旧接口兼容层，正式链路使用 bme_server_client 的 /local/v1/sensor。 */
#ifndef SERVER_UPLOAD_BRIDGE_ENDPOINT
#define SERVER_UPLOAD_BRIDGE_ENDPOINT "/local/v1/sensor"
#endif

#ifndef SERVER_UPLOAD_BRIDGE_TIMEOUT_MS
#define SERVER_UPLOAD_BRIDGE_TIMEOUT_MS 5000U // 单次上传 HTTP 超时，单位 ms。
#endif

#ifndef SERVER_UPLOAD_BRIDGE_JSON_BUFFER_SIZE
#define SERVER_UPLOAD_BRIDGE_JSON_BUFFER_SIZE 256U // 上传 JSON body 缓存大小。
#endif

typedef struct {
    const char *device_id;
    float temperature;
    float humidity;
    float pressure;
    float gas_resistance;
} server_upload_bme690_data_t;

/** 调用方法：系统启动或 BME 上传服务初始化时调用一次，当前无状态，可重复调用。 */
esp_err_t server_upload_bridge_init(void);

/** 调用方法：填好 device_id 和 BME690 数据后调用；正式链路不再使用本旧桥。 */
esp_err_t server_upload_bridge_upload_bme690(const server_upload_bme690_data_t *data);

#ifdef __cplusplus
}
#endif

#endif /* SERVER_UPLOAD_BRIDGE_H */
