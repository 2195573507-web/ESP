#ifndef BME_SENSOR_SERVICE_H
#define BME_SENSOR_SERVICE_H

/**
 * @file bme_sensor_service.h
 * @brief C5 终端 BME690 后台服务接口。
 *
 * app_orchestrator_start() 在 WiFi/系统服务后注册本模块；C5 BME worker 驱动单次
 * tick，voice_chain 在语音 turn 前后通过 pause/wait_paused/resume 协调网络和 heap。
 * BME_SENSOR_DEVICE_ID 是传感模块
 * sensor_id，不是整机 device_id。
 */

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef BME_SENSOR_READ_UPLOAD_PERIOD_MS
#define BME_SENSOR_READ_UPLOAD_PERIOD_MS 5000U // 读取并上传的周期，单位 ms。
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
#define BME_SENSOR_DEVICE_ID "bme690_01" // BME690 模块 sensor_id；整机 device_id 使用 server_comm_get_device_id()。
#endif

/** Fixed-value LCD/status snapshot; it never exposes driver-owned pointers. */
typedef struct {
    bool valid;
    float temperature_c;
    float humidity_percent;
    uint16_t air_quality_score;
    uint32_t generation;
} bme_sensor_service_reading_t;

/**
 * @brief 注册 BME690 event-worker 驱动读取/上传服务。
 *
 * 调用位置：app_orchestrator_start()。
 * 调用时机：WiFi 稳定、system_service 初始化后调用一次；重复调用不会重复创建任务。
 * 输入参数：无。
 * @return ESP_OK 表示服务已注册或已在运行。
 * 失败处理：orchestrator 记录错误，语音和命令链路仍按各自配置继续。
 */
esp_err_t bme_sensor_service_start(void);

/**
 * @brief BME worker 调用：执行一次 BME690 read/quality/update/upload。
 *
 * 调用前和函数内部都会经过 c5_should_run(C5_TASK_TYPE_BME_SENSOR) gate；函数不做
 * 固定周期 delay，也不创建独立 service loop。
 */
esp_err_t bme_sensor_service_tick(void);

/**
 * @brief 暂停 BME690 后台读取/上传。
 *
 * 调用位置：app_runtime_pause_non_voice()。
 * 调用时机：语音链路确认有效人声、即将进入 server voice turn 前。
 * 输入参数：无。
 * 返回值：无；服务未运行时不做处理。
 * 失败处理：无同步错误返回，上层随后用 wait_paused() 判断是否真正让出资源。
 */
void bme_sensor_service_pause(void);

/**
 * @brief 等待 BME690 后台服务进入暂停空闲态。
 *
 * 调用方法：语音独占 gate 在调用 pause() 后调用；返回 ESP_OK 表示 BME 当前未处于
 * 读数或上传临界段。函数不会停止任务，不改底层驱动或服务器接口。
 *
 * @param timeout_ms 最大等待时间，单位 ms。
 * @return 已暂停且空闲返回 ESP_OK；等待超时返回 ESP_ERR_TIMEOUT。
 * 失败处理：voice/runtime 记录超时并按当前策略决定是否继续语音 turn。
 */
esp_err_t bme_sensor_service_wait_paused(uint32_t timeout_ms);

/**
 * @brief 恢复 BME690 后台读取/上传。
 *
 * 调用位置：app_runtime_resume_non_voice()。
 * 调用时机：服务器 PCM 播放结束，语音链路恢复 Mic 监听后。
 * 输入参数：无。
 * 返回值：无；服务未运行时不做处理。
 * 失败处理：无同步错误返回。
 */
void bme_sensor_service_resume(void);

/**
 * @brief 停止 BME690 服务任务。
 *
 * 调用位置：调试或关机流程预留入口；voice_chain 正常只使用 pause/resume。
 * 输入参数：无。
 * 返回值：无；超时只记录日志，不强制删除任务。
 */
void bme_sensor_service_stop(void);

/** @brief 查询服务任务是否已启动；诊断或状态页调用，返回 true/false。 */
bool bme_sensor_service_is_running(void);

/** @brief 查询当前是否处于暂停态；voice/runtime 诊断调用，返回 true/false。 */
bool bme_sensor_service_is_paused(void);

/** Copy the last successful sensor/air-quality reading without I2C or network I/O. */
void bme_sensor_service_get_latest_reading(bme_sensor_service_reading_t *out_reading);

#ifdef __cplusplus
}
#endif

#endif /* BME_SENSOR_SERVICE_H */
