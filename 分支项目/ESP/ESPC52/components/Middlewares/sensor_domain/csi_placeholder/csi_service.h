#ifndef CSI_SERVICE_H
#define CSI_SERVICE_H

/**
 * @file csi_service.h
 * @brief C5 终端 CSI runtime 接口。
 *
 * MAIN_ENABLE_CSI_SERVICE=0 时本模块保持旧行为，不配置 WiFi CSI、不启动任务。
 * MAIN_ENABLE_CSI_SERVICE=1 时启动轻量 CSI callback，并由 C5 worker 通过 event 驱动
 * feature process/report tick，只输出 frame_energy/variance/cv/quality 和本地
 * IDLE/MOTION edge state，不上传 raw CSI。
 */

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化 CSI 服务状态。
 *
 * 调用位置：app_orchestrator_start() 的 CSI 开关分支。
 * 调用时机：WiFi 稳定、system_service 初始化后。
 * 输入参数：无。
 * @return ESP_OK 表示本地状态已准备；失败时上层只记录，不影响 BME/voice/command。
 */
esp_err_t csi_service_init(void);

/** @brief 根据总开关启动 CSI callback；关闭时保持旧行为并返回 ESP_OK。 */
esp_err_t csi_service_start(void);

/** @brief CSI worker 调用：处理一帧 latest pending CSI sample，成功/无数据均不阻塞。 */
esp_err_t csi_service_process_tick(void);

/** @brief CSI worker 调用：发布最近一次 ready feature，不做 raw CSI 输出。 */
esp_err_t csi_service_report_tick(void);

/** @brief 调试预留：执行一次 process + report tick。 */
esp_err_t csi_service_tick(void);

/** @brief 暂停 CSI 摘要上报；不影响 register、heartbeat、BME690、voice、command。 */
void csi_service_pause(void);

/**
 * @brief Mask CSI ingress, clear pending work, and wait for active worker processing.
 *
 * This is the voice-lease ACK path. It does not alter CSI algorithms or payloads.
 */
esp_err_t csi_service_pause_and_wait(uint32_t timeout_ms);

/** @brief 恢复 CSI 摘要上报；只有服务已启动时才生效。 */
void csi_service_resume(void);

#ifdef __cplusplus
}
#endif

#endif /* CSI_SERVICE_H */
