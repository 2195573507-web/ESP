#ifndef GATEWAY_WIFI_H
#define GATEWAY_WIFI_H

/**
 * @file gateway_wifi.h
 * @brief S3 网关 WiFi 状态接口。
 *
 * S3 同时提供 SoftAP 给 C5，并可通过 STA 访问 ESP-server；本模块只暴露启动和状态查询。
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 本地 C5 ingest ready 门控；true 表示 SoftAP/local socket 可接收 C5 流量。 */
extern volatile bool g_net_ready;

/**
 * @brief 启动 S3 APSTA 网络。
 *
 * 调用位置：gateway_orchestrator_start()。
 * 调用时机：registry/router/proxy 初始化后；local HTTP 由 network_worker 在 SoftAP ready 后启动。
 * 输入参数：无。
 * @return ESP_OK 表示 WiFi 已启动或已在运行；NVS/netif/WiFi 配置失败返回对应错误码。
 * 失败处理：回滚本次创建的 WiFi/netif/handler/STA 辅助资源并返回错误；orchestrator 记录降级后继续独立模块。
 */
esp_err_t gateway_wifi_start(void);
/** @brief worker 线程内启动一次非阻塞 STA 扫描；完成事件会由 WiFi callback 转交 worker。 */
esp_err_t gateway_wifi_start_sta_scan(void);
/** @brief worker 在线程上下文读取扫描结果、记录已知 AP，并建立 RSSI 排序候选列表。 */
esp_err_t gateway_wifi_collect_sta_scan_candidates(size_t *out_scan_count);
/** @brief worker 从最近扫描结果选择 STA 候选；avoid_current 为 true 时优先选择另一已知 SSID。 */
esp_err_t gateway_wifi_select_sta_candidate(bool avoid_current);
/** @brief worker 取消超时的 STA 扫描；不会停止 WiFi driver 或 SoftAP。 */
esp_err_t gateway_wifi_cancel_sta_scan(void);
/** @brief 最近一次扫描是否已提供可连接的已知 AP 候选。 */
bool gateway_wifi_has_sta_candidate(void);
/** @brief 记录当前选中候选不可用，供 worker 的 fallback 策略诊断使用。 */
void gateway_wifi_log_sta_candidate_fallback(const char *reason);
/** @brief worker 使用当前已选 STA 候选发起连接。 */
esp_err_t gateway_wifi_connect_sta_current(void);
/** @brief 在非 callback 上下文按 DHCP IPv4 查询当前 SoftAP station MAC。 */
bool gateway_wifi_get_ap_client_mac(const char *peer_ip, uint8_t out_mac[6]);
/** @brief 在非 callback 上下文按 station MAC 查询 SoftAP DHCP lease IPv4。 */
bool gateway_wifi_get_ap_client_ip(const uint8_t mac[6], char *out, size_t out_size);
/** @brief 获取调用时的 SoftAP station 数快照；查询失败时不把结果解释为零。 */
esp_err_t gateway_wifi_get_ap_station_count(size_t *out_count);
/** @brief worker 线程内更新本地 ingest ready gate。 */
void gateway_wifi_set_net_ready_gate(bool ready, const char *reason);
/** @brief 查询 SoftAP 是否已启动；health/heartbeat 日志调用。 */
bool gateway_wifi_is_softap_ready(void);
/** @brief 查询本地 C5->S3 HTTP/UDP ingest 是否可用；不依赖 ESP-server。 */
bool gateway_wifi_is_local_ingest_ready(void);
/** @brief 查询 STA driver 是否已启动；network_worker/gateway health 调用。 */
bool gateway_wifi_is_sta_started(void);
/** @brief 查询 STA 是否已连上上游网络；server_client 发起请求前调用。 */
bool gateway_wifi_is_sta_connected(void);
/** @brief 查询 STA 是否已获得 IPv4 地址；network_worker 的 LINK_STABLE gate 调用。 */
bool gateway_wifi_is_sta_ip_ready(void);
/** @brief 返回最近一次 STA GOT_IP 创建的单调网络 epoch；0 表示尚未获得过地址。 */
uint32_t gateway_wifi_get_sta_network_epoch(void);
/** @brief 返回当前选中的 STA 凭据索引；network_worker 的重连预算诊断调用。 */
size_t gateway_wifi_get_sta_credential_index(void);
/** @brief 记录当前 STA 凭据的一次连接失败并返回该凭据累计 retry_count。 */
uint32_t gateway_wifi_record_sta_credential_failure(void);
/** @brief STA 获得 IPv4 后清除所有候选凭据的失败预算。 */
void gateway_wifi_reset_sta_credential_failures(void);
/** @brief 兼容接口：等价于 gateway_wifi_is_local_ingest_ready()。 */
bool gateway_wifi_is_net_ready(void);

#ifdef __cplusplus
}
#endif

#endif /* GATEWAY_WIFI_H */
