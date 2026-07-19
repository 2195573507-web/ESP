#ifndef LD2450_UART_DIAGNOSTICS_H
#define LD2450_UART_DIAGNOSTICS_H

#include <stdint.h>

typedef struct {
    uint32_t read_timeout;
    uint32_t read_zero;
    uint32_t read_driver_error;
    uint32_t fifo_overflow;
    uint32_t queue_full;
} ld2450_uart_diagnostics_t;

#endif
