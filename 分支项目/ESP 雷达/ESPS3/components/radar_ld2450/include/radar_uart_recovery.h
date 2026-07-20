#ifndef RADAR_UART_RECOVERY_H
#define RADAR_UART_RECOVERY_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RADAR_UART_RECOVERY_OFFLINE = 0,
    RADAR_UART_RECOVERY_BACKOFF,
    RADAR_UART_RECOVERY_WAITING_VALID,
    RADAR_UART_RECOVERY_VALID,
} radar_uart_recovery_state_t;

typedef struct {
    uint32_t error_threshold;
    uint32_t silent_timeout_ms;
    uint32_t backoff_initial_ms;
    uint32_t backoff_max_ms;
    uint32_t valid_frames_required;
} radar_uart_recovery_config_t;

typedef struct {
    radar_uart_recovery_config_t config;
    uint32_t recovery_count;
    uint32_t init_failure_count;
    uint32_t retry_count;
    uint32_t consecutive_error_count;
    uint32_t consecutive_no_valid_count;
    uint32_t consecutive_no_rx_timeout_count;
    uint32_t consecutive_valid_count;
    uint32_t current_backoff_ms;
    uint64_t next_retry_ms;
    uint64_t last_rx_ms;
    uint32_t last_recovery_partial_length;
    uint32_t last_recovery_discarded_bytes;
    uint64_t last_recovery_rx_ms;
    radar_uart_recovery_state_t state;
} radar_uart_recovery_t;

radar_uart_recovery_config_t radar_uart_recovery_default_config(void);
void radar_uart_recovery_init(radar_uart_recovery_t *recovery,
                              const radar_uart_recovery_config_t *config,
                              uint64_t now_ms);
void radar_uart_recovery_note_init_result(radar_uart_recovery_t *recovery,
                                          bool success,
                                          uint64_t now_ms);
void radar_uart_recovery_note_error(radar_uart_recovery_t *recovery,
                                    uint64_t now_ms);
void radar_uart_recovery_note_overflow(radar_uart_recovery_t *recovery,
                                       uint64_t now_ms);
void radar_uart_recovery_note_rx_bytes(radar_uart_recovery_t *recovery,
                                       uint64_t now_ms,
                                       uint32_t byte_count);
void radar_uart_recovery_note_timeout(radar_uart_recovery_t *recovery,
                                      uint64_t now_ms);
void radar_uart_recovery_record_snapshot(radar_uart_recovery_t *recovery,
                                         uint32_t partial_length,
                                         uint32_t discarded_bytes,
                                         uint64_t last_rx_ms);
void radar_uart_recovery_note_no_valid(radar_uart_recovery_t *recovery,
                                       uint64_t now_ms);
void radar_uart_recovery_note_valid_frame(radar_uart_recovery_t *recovery,
                                          uint64_t now_ms);
bool radar_uart_recovery_should_stop_rx(const radar_uart_recovery_t *recovery);
bool radar_uart_recovery_retry_due(const radar_uart_recovery_t *recovery,
                                   uint64_t now_ms);
uint32_t radar_uart_recovery_delay_ms(const radar_uart_recovery_t *recovery,
                                      uint64_t now_ms);
#ifdef __cplusplus
}
#endif

#endif
