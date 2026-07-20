#ifndef RADAR_LOCAL_HANDLER_H
#define RADAR_LOCAL_HANDLER_H

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t radar_local_handler(httpd_req_t *req);
esp_err_t radar_debug_handler(httpd_req_t *req);

#ifdef __cplusplus
}
#endif

#endif
