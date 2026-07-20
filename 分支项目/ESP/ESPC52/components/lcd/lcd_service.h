#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t lcd_service_start(void);
bool lcd_service_is_started(void);

#ifdef __cplusplus
}
#endif