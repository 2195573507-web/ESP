#ifndef CSI_SERVICE_H
#define CSI_SERVICE_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 调用方法：系统服务初始化阶段调用一次；当前 CSI service 仅预留接口，可重复调用。 */
esp_err_t csi_service_init(void);

/** 调用方法：未来接入 CSI 采集后台任务时调用；当前只记录启动状态。 */
esp_err_t csi_service_start(void);

/** 调用方法：语音独占或低 heap 场景暂停 CSI 后台采集；当前只更新预留状态。 */
void csi_service_pause(void);

/** 调用方法：语音链路结束后恢复 CSI 后台采集；当前只更新预留状态。 */
void csi_service_resume(void);

#ifdef __cplusplus
}
#endif

#endif /* CSI_SERVICE_H */
