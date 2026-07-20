#ifndef OFFLINE_POLICY_H
#define OFFLINE_POLICY_H

/**
 * @file offline_policy.h
 * @brief S3 网关 Server 离线状态记录接口。
 *
 * 本模块只记录和映射错误，不负责重试、缓存或业务降级决策。
 */

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 初始化离线状态；gateway_orchestrator_start() 调用。 */
void offline_policy_init(void);
/** @brief 记录一次 Server 请求结果；sensor/voice/command 转发后调用。 */
void offline_policy_record_server_result(esp_err_t ret, int http_status);
/** @brief 查询最近一次 Server 路径是否成功；health/heartbeat 日志调用。 */
bool offline_policy_server_available(void);
/** @brief 获取最近一次错误码字符串；无错误返回空字符串。 */
const char *offline_policy_last_error_code(void);
/** @brief 将 esp_err_t/http_status 映射为协议错误码字符串；voice_proxy/aggregator 调用。 */
const char *offline_policy_code_for_result(esp_err_t ret, int http_status);
/** @brief 获取连续失败次数；诊断日志调用。 */
uint32_t offline_policy_failure_count(void);

#ifdef __cplusplus
}
#endif

#endif /* OFFLINE_POLICY_H */
