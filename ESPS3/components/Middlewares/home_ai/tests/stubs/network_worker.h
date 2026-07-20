#ifndef NETWORK_WORKER_H
#define NETWORK_WORKER_H

#include "esp_err.h"

esp_err_t network_worker_submit_home_ai_events(char *json_body, const char *source);
esp_err_t network_worker_submit_home_ai_virtual_state(char *json_body, const char *source);

#endif /* NETWORK_WORKER_H */
