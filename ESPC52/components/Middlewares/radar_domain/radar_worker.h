#ifndef RADAR_WORKER_H
#define RADAR_WORKER_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"

esp_err_t radar_domain_start(void);
void radar_domain_notify(const uint8_t *data, size_t length, uint64_t timestamp_ms);
void radar_domain_set_link_state(uint8_t link_state, bool online);
void radar_domain_mark_timeout(uint64_t timestamp_ms);

#endif
