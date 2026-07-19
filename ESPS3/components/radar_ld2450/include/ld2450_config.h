#ifndef LD2450_CONFIG_H
#define LD2450_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LD2450_CONFIG_COMMAND_ENABLE 0x00FFU
#define LD2450_CONFIG_COMMAND_DISABLE 0x00FEU
#define LD2450_CONFIG_COMMAND_SINGLE_TARGET 0x0080U
#define LD2450_CONFIG_COMMAND_MULTI_TARGET 0x0090U
#define LD2450_CONFIG_COMMAND_QUERY_TRACKING 0x0091U
#define LD2450_CONFIG_COMMAND_QUERY_VERSION 0x00A0U

size_t ld2450_config_build_command(uint16_t command,
                                   const uint8_t *value,
                                   size_t value_len,
                                   uint8_t *out,
                                   size_t out_size);
bool ld2450_config_ack_matches(const uint8_t *data,
                               size_t data_len,
                               uint16_t command,
                               const uint8_t **out_value,
                               size_t *out_value_len);
esp_err_t ld2450_config_execute(uint16_t command,
                                const uint8_t *value,
                                size_t value_len,
                                uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif

