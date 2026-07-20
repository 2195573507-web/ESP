#include "radar_uart_recovery.h"

#include <limits.h>
#include <string.h>

#include "radar_config.h"

/*
 * UART 恢复状态机把连续错误、长时间无有效帧和重试退避统一处理。
 * 只有连续收到足够有效帧后才恢复为 VALID，避免短暂重连造成在线状态抖动。
 */

static void sat_inc_u32(uint32_t *value)
{
    if (value != NULL && *value < UINT32_MAX) {
        ++(*value);
    }
}

static uint32_t bounded_double(uint32_t value, uint32_t max_value)
{
    if (value >= max_value || value > max_value / 2U) {
        return max_value;
    }
    return value * 2U;
}

radar_uart_recovery_config_t radar_uart_recovery_default_config(void)
{
    const radar_uart_recovery_config_t config = {
        .error_threshold = RADAR_CONFIG_UART_ERROR_THRESHOLD,
        .silent_timeout_ms = RADAR_CONFIG_UART_SILENT_TIMEOUT_MS,
        .backoff_initial_ms = RADAR_CONFIG_UART_BACKOFF_INITIAL_MS,
        .backoff_max_ms = RADAR_CONFIG_UART_BACKOFF_MAX_MS,
        .valid_frames_required = RADAR_CONFIG_UART_VALID_FRAMES_REQUIRED,
    };
    return config;
}

static radar_uart_recovery_config_t sanitize(const radar_uart_recovery_config_t *input)
{
    radar_uart_recovery_config_t config =
        input != NULL ? *input : radar_uart_recovery_default_config();
    const radar_uart_recovery_config_t defaults = radar_uart_recovery_default_config();
    if (config.error_threshold == 0U) config.error_threshold = defaults.error_threshold;
    if (config.silent_timeout_ms == 0U) config.silent_timeout_ms = defaults.silent_timeout_ms;
    if (config.backoff_initial_ms == 0U) config.backoff_initial_ms = defaults.backoff_initial_ms;
    if (config.backoff_max_ms < config.backoff_initial_ms) config.backoff_max_ms = defaults.backoff_max_ms;
    if (config.valid_frames_required == 0U) config.valid_frames_required = defaults.valid_frames_required;
    return config;
}

static void enter_backoff(radar_uart_recovery_t *recovery, uint64_t now_ms)
{
    if (recovery == NULL || recovery->state == RADAR_UART_RECOVERY_BACKOFF) {
        return;
    }
    if (recovery->current_backoff_ms == 0U) {
        recovery->current_backoff_ms = recovery->config.backoff_initial_ms;
    }
    recovery->state = RADAR_UART_RECOVERY_BACKOFF;
    recovery->next_retry_ms = now_ms + recovery->current_backoff_ms;
    recovery->consecutive_valid_count = 0U;
    sat_inc_u32(&recovery->recovery_count);
}

void radar_uart_recovery_init(radar_uart_recovery_t *recovery,
                              const radar_uart_recovery_config_t *config,
                              uint64_t now_ms)
{
    if (recovery == NULL) {
        return;
    }
    memset(recovery, 0, sizeof(*recovery));
    recovery->config = sanitize(config);
    recovery->current_backoff_ms = recovery->config.backoff_initial_ms;
    recovery->next_retry_ms = now_ms;
    recovery->state = RADAR_UART_RECOVERY_OFFLINE;
    recovery->last_rx_ms = now_ms;
}

void radar_uart_recovery_note_init_result(radar_uart_recovery_t *recovery,
                                          bool success,
                                          uint64_t now_ms)
{
    if (recovery == NULL) {
        return;
    }
    if (success) {
        recovery->state = RADAR_UART_RECOVERY_WAITING_VALID;
        recovery->consecutive_error_count = 0U;
        recovery->consecutive_no_valid_count = 0U;
        recovery->consecutive_no_rx_timeout_count = 0U;
        recovery->consecutive_valid_count = 0U;
        recovery->last_rx_ms = now_ms;
        recovery->next_retry_ms = 0U;
        return;
    }
    sat_inc_u32(&recovery->init_failure_count);
    sat_inc_u32(&recovery->retry_count);
    recovery->state = RADAR_UART_RECOVERY_OFFLINE;
    recovery->next_retry_ms = now_ms;
    enter_backoff(recovery, now_ms);
    recovery->current_backoff_ms = bounded_double(recovery->current_backoff_ms,
                                                   recovery->config.backoff_max_ms);
}

void radar_uart_recovery_note_error(radar_uart_recovery_t *recovery,
                                    uint64_t now_ms)
{
    if (recovery == NULL || recovery->state == RADAR_UART_RECOVERY_BACKOFF) {
        return;
    }
    sat_inc_u32(&recovery->consecutive_error_count);
    recovery->consecutive_valid_count = 0U;
    if (recovery->consecutive_error_count >= recovery->config.error_threshold) {
        enter_backoff(recovery, now_ms);
    }
}

void radar_uart_recovery_note_overflow(radar_uart_recovery_t *recovery,
                                       uint64_t now_ms)
{
    if (recovery == NULL || recovery->state == RADAR_UART_RECOVERY_BACKOFF) {
        return;
    }
    sat_inc_u32(&recovery->consecutive_error_count);
    enter_backoff(recovery, now_ms);
}

void radar_uart_recovery_note_rx_bytes(radar_uart_recovery_t *recovery,
                                       uint64_t now_ms,
                                       uint32_t byte_count)
{
    if (recovery == NULL || byte_count == 0U) {
        return;
    }
    recovery->last_rx_ms = now_ms;
    recovery->consecutive_no_rx_timeout_count = 0U;
}

void radar_uart_recovery_note_timeout(radar_uart_recovery_t *recovery,
                                      uint64_t now_ms)
{
    if (recovery == NULL || recovery->state == RADAR_UART_RECOVERY_BACKOFF) {
        return;
    }
    sat_inc_u32(&recovery->consecutive_no_rx_timeout_count);
    if (now_ms >= recovery->last_rx_ms &&
        now_ms - recovery->last_rx_ms >= recovery->config.silent_timeout_ms) {
        enter_backoff(recovery, now_ms);
    }
}

void radar_uart_recovery_record_snapshot(radar_uart_recovery_t *recovery,
                                         uint32_t partial_length,
                                         uint32_t discarded_bytes,
                                         uint64_t last_rx_ms)
{
    if (recovery == NULL) {
        return;
    }
    recovery->last_recovery_partial_length = partial_length;
    recovery->last_recovery_discarded_bytes = discarded_bytes;
    recovery->last_recovery_rx_ms = last_rx_ms;
}

void radar_uart_recovery_note_no_valid(radar_uart_recovery_t *recovery,
                                       uint64_t now_ms)
{
    (void)now_ms;
    if (recovery == NULL || recovery->state == RADAR_UART_RECOVERY_BACKOFF) {
        return;
    }
    sat_inc_u32(&recovery->consecutive_no_valid_count);
    recovery->consecutive_valid_count = 0U;
}

void radar_uart_recovery_note_valid_frame(radar_uart_recovery_t *recovery,
                                          uint64_t now_ms)
{
    if (recovery == NULL || recovery->state == RADAR_UART_RECOVERY_BACKOFF) {
        return;
    }
    recovery->consecutive_error_count = 0U;
    recovery->consecutive_no_valid_count = 0U;
    recovery->consecutive_no_rx_timeout_count = 0U;
    recovery->last_rx_ms = now_ms;
    sat_inc_u32(&recovery->consecutive_valid_count);
    if (recovery->consecutive_valid_count >= recovery->config.valid_frames_required) {
        recovery->state = RADAR_UART_RECOVERY_VALID;
        recovery->current_backoff_ms = recovery->config.backoff_initial_ms;
    }
}

bool radar_uart_recovery_should_stop_rx(const radar_uart_recovery_t *recovery)
{
    return recovery != NULL && recovery->state == RADAR_UART_RECOVERY_BACKOFF;
}

bool radar_uart_recovery_retry_due(const radar_uart_recovery_t *recovery,
                                   uint64_t now_ms)
{
    return recovery != NULL && recovery->state == RADAR_UART_RECOVERY_BACKOFF &&
           now_ms >= recovery->next_retry_ms;
}

uint32_t radar_uart_recovery_delay_ms(const radar_uart_recovery_t *recovery,
                                      uint64_t now_ms)
{
    if (recovery == NULL || recovery->state != RADAR_UART_RECOVERY_BACKOFF ||
        now_ms >= recovery->next_retry_ms) {
        return 0U;
    }
    const uint64_t remaining = recovery->next_retry_ms - now_ms;
    return remaining > UINT32_MAX ? UINT32_MAX : (uint32_t)remaining;
}
