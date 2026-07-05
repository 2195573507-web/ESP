#ifndef GATEWAY_EVENT_REPORTER_H
#define GATEWAY_EVENT_REPORTER_H

/**
 * @file gateway_event_reporter.h
 * @brief S3 网关 system log / alarm 上报接口。
 *
 * 本模块只把 S3 侧真实状态变化写入 ESP-server logs API。C5 不直接调用 logs API。
 */

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

void gateway_event_reporter_init(void);

esp_err_t gateway_event_reporter_system(const char *device_id,
                                        const char *level,
                                        const char *message,
                                        const char *reason);

esp_err_t gateway_event_reporter_alarm(const char *device_id,
                                       const char *level,
                                       const char *title,
                                       const char *message,
                                       const char *reason);

void gateway_event_reporter_record_server_state(bool available);

#ifdef __cplusplus
}
#endif

#endif /* GATEWAY_EVENT_REPORTER_H */
