#ifndef SMART_HOME_GATEWAY_H
#define SMART_HOME_GATEWAY_H

/**
 * @file smart_home_gateway.h
 * @brief S3 智能家居 pending/ack 网关。
 *
 * 当前固件没有真实智能家居执行器。本模块从 Server 领取 pending 命令，交给
 * Home AI 固定容量虚拟执行器，并明确以 virtual/verified 字段回传本地状态事实。
 */

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t smart_home_gateway_init(void);
void smart_home_gateway_poll_once(void);

#ifdef __cplusplus
}
#endif

#endif /* SMART_HOME_GATEWAY_H */
