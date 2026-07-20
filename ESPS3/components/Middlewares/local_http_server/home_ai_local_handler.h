#ifndef HOME_AI_LOCAL_HANDLER_H
#define HOME_AI_LOCAL_HANDLER_H

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Fixed-body C5 voice lock endpoint; it intentionally bypasses the telemetry queue. */
esp_err_t home_ai_voice_session_handler(httpd_req_t *req);
/* Fixed-text offline command ingress; C5 forwards tokens but never interprets them. */
esp_err_t home_ai_offline_voice_command_handler(httpd_req_t *req);

#ifdef __cplusplus
}
#endif

#endif /* HOME_AI_LOCAL_HANDLER_H */
