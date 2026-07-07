#ifndef CSI_SERVER_CLIENT_H
#define CSI_SERVER_CLIENT_H

/**
 * @file csi_server_client.h
 * @brief C5 CSI feature upload interface.
 *
 * This client only publishes low-dimensional feature frames to ESPS3
 * /local/v1/csi/result. It does not upload raw CSI, I/Q, phase, selected
 * subcarriers, or C5-side state decisions.
 */

#include <stdbool.h>
#include <stddef.h>

#include "csi_feature.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t csi_server_client_init(void);

const char *csi_server_client_local_link_id(void);

esp_err_t csi_server_client_format_feature_result(const csi_feature_frame_t *result,
                                                  char *json_body,
                                                  size_t json_body_size);

esp_err_t csi_server_client_publish_feature_result(const csi_feature_frame_t *result,
                                                   bool log_enabled,
                                                   bool http_enabled);

esp_err_t csi_server_client_upload_feature_result(const csi_feature_frame_t *result);

#ifdef __cplusplus
}
#endif

#endif /* CSI_SERVER_CLIENT_H */
