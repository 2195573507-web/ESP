#ifndef GATEWAY_ORCHESTRATOR_H
#define GATEWAY_ORCHESTRATOR_H

/**
 * @file gateway_orchestrator.h
 * @brief S3 网关启动编排入口。
 *
 * 本头文件只暴露 gateway_orchestrator_start() 给 main/app_main.c；SoftAP、HTTP、
 * registry、voice、command、sensor 和 CSI placeholder 的细节由各模块内部维护。
 */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动 S3 网关所有后台服务。
 *
 * 调用位置：main/gateway_startup_task()。
 * 调用时机：app_main 创建启动任务后立即调用，一次调用后函数保持 heartbeat idle loop。
 * 输入参数：无。
 * 返回值：无；启动失败的关键 ESP_ERROR_CHECK 会进入 ESP-IDF 错误处理。
 * 失败处理：可降级的 Server 可用性由 offline_policy 记录，HTTP handler 继续返回本地错误码。
 */
void gateway_orchestrator_start(void);

#ifdef __cplusplus
}
#endif

#endif /* GATEWAY_ORCHESTRATOR_H */
