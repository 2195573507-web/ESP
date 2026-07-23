#ifndef COMMAND_ROUTER_H
#define COMMAND_ROUTER_H

/**
 * @file command_router.h
 * @brief S3 网关命令队列与 C5 本地命令映射接口。
 *
 * Server 完整命令在 S3 映射为 C5 本地 c/cid/a/seq/ttl_ms，C5 执行后再由本模块
 * 把 ok/e ack 映射回 Server 错误码字符串。
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 初始化命令队列锁和本地状态；gateway_orchestrator_start() 调用。 */
esp_err_t command_router_init(void);
/** @brief 暂停指定 C5 的 Server pending 拉取和本地命令下发；幂等且不清队列。 */
esp_err_t command_router_suspend_peer(const char *device_id);
/** @brief 恢复指定 C5 的命令资源；幂等且不改变已有队列内容。 */
esp_err_t command_router_restore_peer(const char *device_id);
/** @brief 返回指定 C5 的命令资源是否已恢复。 */
bool command_router_peer_active(const char *device_id);
/** @brief 返回是否至少有一个 C5 的命令资源处于 active。 */
bool command_router_has_active_peers(void);
/** @brief 入队一条本地或 Server 命令；调试/Server pending ingest 调用。 */
esp_err_t command_router_enqueue(const char *target_device_id,
                                 const char *command_type,
                                 const char *params_json,
                                 const char *source);
/**
 * @brief 入队本地命令并返回分配的 command ID。
 *
 * 仅供需要把本地异步状态与 pending command 绑定的调用方使用；不会改变 Server
 * command 的 ID 或 ACK 流程。
 */
esp_err_t command_router_enqueue_with_id(const char *target_device_id,
                                         const char *command_type,
                                         const char *params_json,
                                         const char *source,
                                         char *out_command_id,
                                         size_t out_command_id_size);
/**
 * @brief Queue the S3-owned command-capture request with its complete session identity.
 *
 * `command_id` is allocated by the session owner before this command becomes visible
 * to C5, preventing an ACK from racing ahead of its S3 session identity. The immutable
 * wake stream correlation is carried with the command; command PCM binds later.
 */
esp_err_t command_router_enqueue_voice_command_capture(const char *target_device_id,
                                                        const char *command_id,
                                                        uint32_t generation,
                                                        uint32_t wake_stream_id,
                                                        uint32_t timeout_ms);
/**
 * @brief Complete the local voice-capture ACK handshake without forwarding it to Server.
 *
 * The ACK is only accepted for a dispatched `voice.start_command_capture` command
 * whose target and generation match.  A negative ACK completes the local command as
 * failed; it never grants permission to open the command capture stream.
 */
esp_err_t command_router_ack_local_voice(const char *command_id,
                                         const char *device_id,
                                         uint32_t generation,
                                         bool accepted);
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
