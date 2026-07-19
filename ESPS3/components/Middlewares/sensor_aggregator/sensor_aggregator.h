#ifndef SENSOR_AGGREGATOR_H
#define SENSOR_AGGREGATOR_H

/**
 * @file sensor_aggregator.h
 * @brief S3 网关 sensor/status 转发接口。
 *
 * 本模块接收已通过 protocol_adapter 校验的完整 envelope，再转发到 ESP-server ingest。
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "protocol_adapter.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool accepted;
    bool forwarded;
    int server_status;
    esp_err_t server_ret;
    const char *error_code;
} sensor_aggregator_result_t;

/** @brief 初始化 sensor/status 聚合器；gateway_orchestrator_start() 调用，当前无状态。 */
void sensor_aggregator_init(void);
/** @brief 暂停指定 C5 的实时 sensor 上云和 cache replay；幂等且保留 latest/history/cache。 */
esp_err_t sensor_aggregator_suspend_peer(const char *device_id);
/** @brief 恢复指定 C5 的 sensor 上云资源；幂等且不修改已有聚合数据。 */
esp_err_t sensor_aggregator_restore_peer(const char *device_id);
/** @brief 返回指定 C5 的 sensor 资源是否已恢复。 */
bool sensor_aggregator_peer_active(const char *device_id);
/** @brief 返回是否至少有一个 C5 的 sensor 资源处于 active。 */
bool sensor_aggregator_has_active_peers(void);
/** @brief 构造并上传一次 dashboard snapshot；gateway periodic task 调用。 */
void sensor_aggregator_upload_snapshot(void);
/** @brief worker 线程内立即构造并上传一次 dashboard snapshot。 */
esp_err_t sensor_aggregator_upload_snapshot_now(void);
/**
 * @brief 处理一条 status 或 sensor envelope。
 *
 * 调用位置：local_http_server 的 status/sensor handler。
 * @param envelope 已补全并校验的 envelope，不能为空。
 * @param result 输出转发结果，不能为空。
 * @return ESP_OK 表示本地已接收；adapter 构造失败等本地错误返回对应错误码。
 * 失败处理：Server 转发失败不会拒绝 C5 本地请求，只在 result/offline_policy 中记录。
 */
esp_err_t sensor_aggregator_handle_envelope(const protocol_adapter_envelope_t *envelope,
                                            sensor_aggregator_result_t *result);
/** @brief 处理统一设备流 sensor frame：v1=sensor_value_1，v2=sensor_value_2，v3=quality。 */
esp_err_t sensor_aggregator_handle_stream_sensor(const char *device_id,
                                                 int64_t timestamp_ms,
                                                 const char *link_id,
                                                 double sensor_value_1,
                                                 double sensor_value_2,
                                                 double quality,
                                                 sensor_aggregator_result_t *result);
/** @brief 处理统一设备流 status frame：v1=heap，v2=uptime，v3=wifi_rssi。 */
esp_err_t sensor_aggregator_handle_stream_status(const char *device_id,
                                                 int64_t timestamp_ms,
                                                 double heap,
                                                 double uptime,
                                                 double wifi_rssi,
                                                 sensor_aggregator_result_t *result);
/** @brief 记录一次 voice turn 事件并尝试上传 dashboard snapshot。 */
void sensor_aggregator_record_voice_event(const char *device_id,
                                          size_t pcm_bytes,
                                          uint32_t duration_ms);
/** @brief 记录一次命令 ack 事件并尝试上传 dashboard snapshot。 */
void sensor_aggregator_record_command_ack(const char *device_id,
                                          const char *command_id,
                                          unsigned int command_code,
                                          bool completed);

#ifdef __cplusplus
}
#endif

#endif /* SENSOR_AGGREGATOR_H */
