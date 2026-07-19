#ifndef CSI_SERVER_CLIENT_H
#define CSI_SERVER_CLIENT_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 调用方法：CSI service 初始化时调用一次；当前仅预留服务器客户端。 */
esp_err_t csi_server_client_init(void);

/** 调用方法：CSI 特征 JSON 准备好后调用；当前未接入服务器，返回 ESP_ERR_NOT_SUPPORTED。 */
esp_err_t csi_server_client_upload_features(const char *device_id,
                                            const char *features_json);

#ifdef __cplusplus
}
#endif

#endif /* CSI_SERVER_CLIENT_H */
