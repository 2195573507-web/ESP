#ifndef APP_TIME_SYNC_H
#define APP_TIME_SYNC_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "server_comm_config.h"

/*
 * 本地网关时间同步配置
 *
 * 调用方法：
 * 1. 如果只改 ESPS3 gateway IP，优先修改 APP_TIME_SYNC_SERVER_BASE_URL。
 * 2. 如果本地时间接口路径变了，修改 APP_TIME_SYNC_TIME_NOW_PATH。
 * 3. app_time_sync_once() 只会把 /local/v1 endpoint 交给 C5 公共 HTTP 层。
 */
#ifndef APP_TIME_SYNC_SERVER_BASE_URL
// 本地网关基础地址，只包含协议、IP 和端口，不要在末尾加接口路径。
#define APP_TIME_SYNC_SERVER_BASE_URL SERVER_COMM_BASE_URL
#endif

#ifndef APP_TIME_SYNC_TIME_NOW_PATH
// 本地时间同步接口预留路径；默认启动链路不调用，未同步时使用 uptime_ms。
#define APP_TIME_SYNC_TIME_NOW_PATH "/local/v1/time/now"
#endif

#ifndef APP_TIME_SYNC_SERVER_URL
// 兼容旧调用方的完整 URL；app_time_sync_once() 会提取 /local/v1 endpoint。
#define APP_TIME_SYNC_SERVER_URL APP_TIME_SYNC_SERVER_BASE_URL APP_TIME_SYNC_TIME_NOW_PATH
#endif

/*
 * HTTP 请求和日志参数
 *
 * 调用方法：
 * - 网络较慢或本地网关偶发超时时，可适当调大 APP_TIME_SYNC_HTTP_TIMEOUT_MS。
 * - 本地网关响应 JSON 变长时，可调大 APP_TIME_SYNC_RESPONSE_BUFFER_SIZE。
 * - URL 很长时，可调大 APP_TIME_SYNC_URL_BUFFER_SIZE。
 * - 日志太长或太短时，可调整 APP_TIME_SYNC_BODY_LOG_PREVIEW_LEN。
 */
#ifndef APP_TIME_SYNC_HTTP_TIMEOUT_MS
// HTTP GET 请求超时时间，单位 ms；超时只会让本次同步失败，不会阻塞 BME690 后续流程。
#define APP_TIME_SYNC_HTTP_TIMEOUT_MS 2000
#endif

#ifndef APP_TIME_SYNC_RESPONSE_BUFFER_SIZE
// 响应体缓存大小，单位字节；超过该大小时只保留前面内容并在日志中标记 overflow。
#define APP_TIME_SYNC_RESPONSE_BUFFER_SIZE 1024
#endif

#ifndef APP_TIME_SYNC_URL_BUFFER_SIZE
// 内部保存 /local/v1 endpoint 的缓存大小，单位字节。
#define APP_TIME_SYNC_URL_BUFFER_SIZE 256
#endif

#ifndef APP_TIME_SYNC_BODY_LOG_PREVIEW_LEN
// 日志中打印响应 body 前多少个字符，用于确认字段和接口路径是否正确。
#define APP_TIME_SYNC_BODY_LOG_PREVIEW_LEN 160
#endif

/*
 * 时间 JSON 字段名配置
 *
 * 当前服务器返回示例：
 * {"ok":true,"server_time_ms":1780395122725,"server_time_iso":"2026-06-02T10:12:02.725Z"}
 *
 * 注意：这里的宏值包含 JSON 字段名两侧的双引号，便于轻量字符串解析。
 */
#ifndef APP_TIME_SYNC_JSON_OK_KEY
// 可选状态字段；如果响应里存在 "ok":false，本次同步会返回 ESP_ERR_INVALID_RESPONSE。
#define APP_TIME_SYNC_JSON_OK_KEY "\"ok\""
#endif

#ifndef APP_TIME_SYNC_JSON_SERVER_TIME_MS_KEY
// 必填时间戳字段，单位 ms；缺少该字段时不会更新本地 offset。
#define APP_TIME_SYNC_JSON_SERVER_TIME_MS_KEY "\"server_time_ms\""
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief WiFi 已连接后，对本地网关时间接口执行一次 HTTP 时间校准。
 *
 * 调用方法：外部在 WiFi connected 之后传入本地 /local/v1 endpoint 或兼容旧完整 URL。
 * 本函数只执行一次请求，不创建后台任务。
 *
 * @param server_time_url 本地时间接口 URL 或 /local/v1 endpoint；公共 HTTP 层只接收 endpoint。
 * @return 成功返回 ESP_OK；失败返回 ESP_FAIL 或 ESP-IDF 具体错误码。
 */
esp_err_t app_time_sync_once(const char *server_time_url);

/**
 * @brief 查询是否已经至少成功完成过一次时间校准。
 *
 * 调用方法：上传传感器数据或打印时间戳前调用，false 时不要依赖 Unix 时间。
 */
bool app_time_sync_is_synced(void);

/**
 * @brief 获取 ESP 从启动到当前的 uptime，单位 ms。
 *
 * 调用方法：需要单调递增本地时间或调试耗时时调用，不依赖服务器同步。
 */
int64_t app_time_sync_get_uptime_ms(void);

/**
 * @brief 获取估算的 Unix 时间戳，单位 ms。
 *
 * 未同步时返回 0；已同步时返回当前 uptime_ms + 最近一次计算出的 offset_ms。
 */
int64_t app_time_sync_get_unix_ms(void);

/**
 * @brief 获取最近一次成功校准计算出的 offset，单位 ms。
 *
 * 调用方法：调试服务器时间和本地 uptime 偏移时调用。
 */
int64_t app_time_sync_get_offset_ms(void);

/**
 * @brief 获取最近一次成功校准的 HTTP 往返耗时，单位 ms。
 *
 * 调用方法：诊断时间同步网络延迟时调用。
 */
int64_t app_time_sync_get_last_rtt_ms(void);

/**
 * @brief 获取最近一次成功校准使用的 ESP 中点 uptime，单位 ms。
 *
 * 调用方法：需要复盘最近一次同步时刻时调用。
 */
int64_t app_time_sync_get_last_sync_uptime_ms(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_TIME_SYNC_H */
