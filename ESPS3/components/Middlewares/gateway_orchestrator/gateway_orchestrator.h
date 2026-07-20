#ifndef GATEWAY_ORCHESTRATOR_H
#define GATEWAY_ORCHESTRATOR_H

/**
 * @file gateway_orchestrator.h
 * @brief S3 网关启动编排入口。
 *
 * 本头文件暴露 gateway_orchestrator_start() 给 main/app_main.c。运行期事件队列、
 * 调度 tick 和 backpressure 由 runtime/s3_scheduler 负责。
 */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动 S3 网关所有后台服务。
 *
 * 调用位置：main/gateway_startup_task()。
 * 调用时机：app_main 创建启动任务后立即调用。
 * 输入参数：无。
 * 返回值：无；每个模块失败均会记录并降级，其他独立模块继续启动。
 * 失败处理：network_worker 管理本地 HTTP ingress 的启动和重试，S3 主流程继续运行。
 */
void gateway_orchestrator_start(void);

#ifdef __cplusplus
}
#endif

#endif /* GATEWAY_ORCHESTRATOR_H */
