#ifndef COMMAND_ROUTER_H
#define COMMAND_ROUTER_H

/**
 * @file command_router.h
 * @brief S3 网关命令队列与 C5 本地命令映射接口。
 *
 * Server 完整命令在 S3 映射为 C5 本地 c/cid/a/seq/ttl_ms，C5 执行后再由本模块
 * 把 ok/e ack 映射回 Server 错误码字符串。
 */

#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 初始化命令队列锁和本地状态；gateway_orchestrator_start() 调用。 */
esp_err_t command_router_init(void);
/** @brief 入队一条本地或 Server 命令；调试/Server pending ingest 调用。 */
esp_err_t command_router_enqueue(const char *target_device_id,
                                 const char *command_type,
                                 const char *params_json,
                                 const char *source);
/** @brief scheduler 调用：从 Server 拉取 pending commands 并写入本地命令队列。 */
void command_router_poll_server_pending(void);
/**
 * @brief 为 C5 构造 pending commands 轻量 JSON。
 *
 * 调用位置：local_http_server 的 /local/v1/commands/pending handler。
 * @param device_id 完整 C5 device_id，不能为空。
 * @param out 输出 JSON 缓冲区。
 * @param out_size 输出缓冲区大小。
 * @return ESP_OK 表示 JSON 已写入；Server 拉取/解析/缓冲区失败返回对应错误码。
 * 失败处理：local_http_server 返回本地 command_poll_failed。
 */
esp_err_t command_router_build_pending_json(const char *device_id, char *out, size_t out_size);
/**
 * @brief 处理 C5 命令 ack 并转发给 Server。
 *
 * 调用位置：local_http_server 的 /local/v1/commands/{id}/ack handler。
 * @param command_id URL 中的命令 ID。
 * @param ack_body C5 轻量 ack JSON。
 * @return ESP_OK 表示本地和 Server ack 完成；解析/转发失败返回对应错误码。
 * 失败处理：local_http_server 返回本地 ack_failed。
 */
esp_err_t command_router_ack(const char *command_id, const char *ack_body);

#ifdef __cplusplus
}
#endif

#endif /* COMMAND_ROUTER_H */
