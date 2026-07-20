#ifndef LD2450_UART_H
#define LD2450_UART_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "ld2450_uart_diagnostics.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool overflow;
    bool buffer_full;
    bool line_error;
} ld2450_uart_events_t;

esp_err_t ld2450_uart_init(void);
esp_err_t ld2450_uart_deinit(void);
esp_err_t ld2450_uart_flush(void);
int ld2450_uart_read(uint8_t *buffer, size_t buffer_size, uint32_t timeout_ms);
int ld2450_uart_write(const uint8_t *data, size_t data_len);
void ld2450_uart_drain_events(ld2450_uart_events_t *out);
void ld2450_uart_get_diagnostics(ld2450_uart_diagnostics_t *out);
bool ld2450_uart_is_enabled(void);

#ifdef __cplusplus
}
#endif

#endif
