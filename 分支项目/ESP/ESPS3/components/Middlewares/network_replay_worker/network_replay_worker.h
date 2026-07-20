#ifndef NETWORK_REPLAY_WORKER_H
#define NETWORK_REPLAY_WORKER_H

/**
 * @file network_replay_worker.h
 * @brief ESPS3 BME cache replay worker.
 */

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t network_replay_worker_init(void);
void network_replay_worker_request_bme_replay(void);

#ifdef __cplusplus
}
#endif

#endif /* NETWORK_REPLAY_WORKER_H */
