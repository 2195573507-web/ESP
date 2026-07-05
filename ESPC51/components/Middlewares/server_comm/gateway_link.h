#ifndef GATEWAY_LINK_H
#define GATEWAY_LINK_H

/**
 * @file gateway_link.h
 * @brief C5 终端到 ESPS3 本地网关的统一链路状态机。
 *
 * C5 与 S3 断联时，恢复 S3 连接必须高于语音、传感器上报、命令轮询等普通业务。
 * 因此所有 /local/v1 普通请求先检查本状态机；只有 reconnect 任务本身可以在
 * LINK_READY 之前执行 WiFi 检查、health probe 和 child register。
 */

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef GATEWAY_LINK_RECONNECT_LOG_INTERVAL_MS
#define GATEWAY_LINK_RECONNECT_LOG_INTERVAL_MS 30000U
#endif

#ifndef GATEWAY_LINK_WAIT_FOREVER_MS
#define GATEWAY_LINK_WAIT_FOREVER_MS UINT32_MAX
#endif

typedef enum {
    LINK_DOWN = 0,
    LINK_WIFI_CONNECTED,
    LINK_REGISTERING,
    LINK_READY,
    LINK_LOST,
} gateway_link_state_t;

typedef void (*gateway_link_voice_abort_cb_t)(const char *reason);

/** @brief 启动 gateway_link 重连任务；app_orchestrator 在 WiFi 初始化后调用。 */
esp_err_t gateway_link_start(void);

/** @brief 等待 S3 health probe 和 child register 全部完成。 */
esp_err_t gateway_link_wait_ready(uint32_t timeout_ms);

/** @brief WiFi STA 断开事件调用：立即进入 LINK_DOWN 并触发重连优先级。 */
void gateway_link_notify_wifi_down(void);

/** @brief WiFi STA 拿到 IP 后调用：进入 LINK_WIFI_CONNECTED 并触发 health/register。 */
void gateway_link_notify_wifi_got_ip(void);

/** @brief 业务或语音清理层注册断联中止回调；状态从 READY 掉出时触发。 */
void gateway_link_set_voice_abort_callback(gateway_link_voice_abort_cb_t callback);

/** @brief 当前状态快照。 */
gateway_link_state_t gateway_link_get_state(void);

/** @brief 状态名用于日志。 */
const char *gateway_link_state_name(gateway_link_state_t state);

/** @brief 只有 LINK_READY 才允许普通业务和新的 server voice turn。 */
bool gateway_link_is_ready(void);

/** @brief 状态非 READY 时即为 reconnect mode。 */
bool gateway_link_in_reconnect_mode(void);

/** @brief 普通任务门控：READY 才返回 true；否则限频打印 reconnect skip 日志。 */
bool gateway_link_can_run_non_voice_task(const char *task_name);

/** @brief 语音启动门控：READY 才返回 true；否则限频提示 gateway offline。 */
bool gateway_link_can_start_voice_turn(void);

/** @brief local_gateway_comm 在每次 HTTP 结束后统一上报结果。 */
void gateway_link_record_http_result(esp_err_t ret, bool voice_request, bool reconnect_request);

/** @brief 判断错误是否属于本地 S3 HTTP 连接类失败。 */
bool gateway_link_http_error_is_link_failure(esp_err_t ret);

/** @brief 标记当前 task 是 reconnect probe/register，允许绕过 READY gate。 */
void gateway_link_set_reconnect_request_active(bool active);

/** @brief 查询当前 task 是否为 reconnect probe/register。 */
bool gateway_link_reconnect_request_is_active_for_current_task(void);

#ifdef __cplusplus
}
#endif

#endif /* GATEWAY_LINK_H */
