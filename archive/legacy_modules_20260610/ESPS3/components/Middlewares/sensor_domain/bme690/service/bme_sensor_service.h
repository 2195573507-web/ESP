#ifndef BME_SENSOR_SERVICE_H
#define BME_SENSOR_SERVICE_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* BME690 后台服务配置：语音独占时会暂停本服务，避免上传占用 heap/网络。 */
#ifndef BME_SENSOR_TASK_STACK
#define BME_SENSOR_TASK_STACK 6144U // BME 后台任务栈，单位字节。
#endif

#ifndef BME_SENSOR_TASK_PRIORITY
#define BME_SENSOR_TASK_PRIORITY 2U // BME 后台任务优先级，低于语音链路。
#endif

#ifndef BME_SENSOR_READ_UPLOAD_PERIOD_MS
#define BME_SENSOR_READ_UPLOAD_PERIOD_MS 2000U // 读取并上传的周期，单位 ms。
#endif

#ifndef BME_SENSOR_PAUSED_DELAY_MS
#define BME_SENSOR_PAUSED_DELAY_MS 400U // 暂停态轮询间隔，单位 ms。
#endif

#ifndef BME_SENSOR_STOP_JOIN_TIMEOUT_MS
#define BME_SENSOR_STOP_JOIN_TIMEOUT_MS 1500U // stop 等待任务退出超时，单位 ms。
#endif

#ifndef BME_SENSOR_WAIT_PAUSED_POLL_MS
#define BME_SENSOR_WAIT_PAUSED_POLL_MS 25U // 等待暂停空闲时的轮询间隔，单位 ms。
#endif

#ifndef BME_SENSOR_DEVICE_ID
#define BME_SENSOR_DEVICE_ID "bme690_01" // BME690 模块 sensor_id；整机 device_id 使用 SERVER_COMM_DEVICE_ID。
#endif

/**
 * @brief 启动 BME690 后台读取/上传服务。
 *
 * 调用方法：Wi-Fi 稳定后调用一次。重复调用不会重复创建任务。
 */
esp_err_t bme_sensor_service_start(void);

/**
 * @brief 暂停 BME690 后台读取/上传。
 *
 * 调用方法：语音链路确认有效人声、即将进入 server voice turn 前调用。
 */
void bme_sensor_service_pause(void);

/**
 * @brief 等待 BME690 后台服务进入暂停空闲态。
 *
 * 调用方法：语音独占 gate 在调用 pause() 后调用；返回 ESP_OK 表示 BME 当前未处于
 * 读数或上传临界段。函数不会停止任务，不改底层驱动或服务器接口。
 *
 * @param timeout_ms 最大等待时间。
 * @return 已暂停且空闲返回 ESP_OK；等待超时返回 ESP_ERR_TIMEOUT。
 */
esp_err_t bme_sensor_service_wait_paused(uint32_t timeout_ms);

/**
 * @brief 恢复 BME690 后台读取/上传。
 *
 * 调用方法：服务器 PCM 播放结束，语音链路恢复 Mic 监听后调用。
 */
void bme_sensor_service_resume(void);

/**
 * @brief 停止 BME690 服务任务。
 *
 * voice_chain 正常只使用 pause/resume；stop 用于调试或未来关机流程。
 */
void bme_sensor_service_stop(void);

/** 调用方法：诊断或状态页查询服务任务是否已启动。 */
bool bme_sensor_service_is_running(void);

/** 调用方法：voice/runtime 诊断当前 BME 是否处于暂停态。 */
bool bme_sensor_service_is_paused(void);

#ifdef __cplusplus
}
#endif

#endif /* BME_SENSOR_SERVICE_H */
