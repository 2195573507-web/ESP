#ifndef SMART_HOME_GATEWAY_H
#define SMART_HOME_GATEWAY_H

/**
 * @file smart_home_gateway.h
 * @brief S3 智能家居 pending/ack 网关。
 *
 * 当前固件没有真实智能家居执行器。本模块只从 Server 领取 pending 命令，并按
 * failed ACK 回传，避免把 mock 状态伪装成真实设备执行成功。
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
