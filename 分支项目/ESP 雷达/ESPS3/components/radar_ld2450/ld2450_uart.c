#include "ld2450_uart.h"

#include <limits.h>
#include <stdbool.h>
#include <string.h>

#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "radar_config.h"

/*
 * UART 层只封装板级串口资源和驱动异常计数。板级引脚未确认时保持禁用，
 * 上层可据此发布离线状态，而不会猜测端口或占用调试串口。
 */

static bool s_driver_installed;
static ld2450_uart_diagnostics_t s_diagnostics;
static QueueHandle_t s_event_queue;

static void sat_inc_u32(uint32_t *value)
{
    if (value != NULL && *value < UINT32_MAX) {
        ++(*value);
    }
}

bool ld2450_uart_is_enabled(void)
{
    return RADAR_CONFIG_UART_ENABLED == 1;
}

esp_err_t ld2450_uart_init(void)
{
    if (!ld2450_uart_is_enabled()) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (RADAR_CONFIG_UART_PORT_INDEX < 0 ||
        RADAR_CONFIG_UART_TX_GPIO < 0 ||
        RADAR_CONFIG_UART_RX_GPIO < 0) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_driver_installed) {
        return ESP_OK;
    }

    const uart_port_t port = (uart_port_t)RADAR_CONFIG_UART_PORT_INDEX;
    const uart_config_t config = {
        .baud_rate = RADAR_CONFIG_UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 0,
        .source_clk = UART_SCLK_DEFAULT,
        .flags = {
            .allow_pd = 0,
            .backup_before_sleep = 0,
        },
    };

    esp_err_t ret = uart_param_config(port, &config);
    if (ret == ESP_OK) {
        ret = uart_set_pin(port,
                           RADAR_CONFIG_UART_TX_GPIO,
                           RADAR_CONFIG_UART_RX_GPIO,
                           UART_PIN_NO_CHANGE,
                           UART_PIN_NO_CHANGE);
    }
    if (ret == ESP_OK) {
        ret = uart_driver_install(port,
                                  RADAR_CONFIG_UART_RX_RING_BYTES,
                                  0,
                                  20,
                                  &s_event_queue,
                                  0);
    }
    if (ret != ESP_OK) {
        return ret;
    }

    s_driver_installed = true;
    return ESP_OK;
}

esp_err_t ld2450_uart_deinit(void)
{
    if (!s_driver_installed) {
        return ESP_OK;
    }
    esp_err_t ret = uart_driver_delete((uart_port_t)RADAR_CONFIG_UART_PORT_INDEX);
    if (ret == ESP_OK) {
        s_driver_installed = false;
        s_event_queue = NULL;
    }
    return ret;
}

esp_err_t ld2450_uart_flush(void)
{
    if (!s_driver_installed) {
        return ESP_OK;
    }
    esp_err_t ret = uart_flush_input((uart_port_t)RADAR_CONFIG_UART_PORT_INDEX);
    return ret;
}

int ld2450_uart_read(uint8_t *buffer, size_t buffer_size, uint32_t timeout_ms)
{
    if (buffer == NULL || buffer_size == 0U) {
        sat_inc_u32(&s_diagnostics.read_driver_error);
        return -1;
    }
    if (!s_driver_installed) {
        sat_inc_u32(&s_diagnostics.read_driver_error);
        return -1;
    }

    int len = uart_read_bytes((uart_port_t)RADAR_CONFIG_UART_PORT_INDEX,
                              buffer,
                              buffer_size,
                              pdMS_TO_TICKS(timeout_ms));
    if (len < 0) {
        sat_inc_u32(&s_diagnostics.read_driver_error);
    } else if (len == 0) {
        sat_inc_u32(timeout_ms == 0U ? &s_diagnostics.read_zero :
                                    &s_diagnostics.read_timeout);
    }
    return len;
}

int ld2450_uart_write(const uint8_t *data, size_t data_len)
{
    if (!s_driver_installed || data == NULL || data_len == 0U || data_len > INT_MAX) {
        return -1;
    }
    int written = uart_write_bytes((uart_port_t)RADAR_CONFIG_UART_PORT_INDEX,
                                   data,
                                   data_len);
    return written;
}

void ld2450_uart_drain_events(ld2450_uart_events_t *out)
{
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
    if (!s_driver_installed || s_event_queue == NULL) {
        return;
    }

    uart_event_t event;
    const uart_port_t port = (uart_port_t)RADAR_CONFIG_UART_PORT_INDEX;
    while (xQueueReceive(s_event_queue, &event, 0) == pdTRUE) {
        if (event.type == UART_FIFO_OVF) {
            sat_inc_u32(&s_diagnostics.fifo_overflow);
            if (out != NULL) {
                out->overflow = true;
            }
            (void)uart_flush_input(port);
        } else if (event.type == UART_BUFFER_FULL) {
            sat_inc_u32(&s_diagnostics.queue_full);
            if (out != NULL) {
                out->buffer_full = true;
            }
            (void)uart_flush_input(port);
        } else if (event.type == UART_BREAK || event.type == UART_PARITY_ERR ||
                   event.type == UART_FRAME_ERR) {
            sat_inc_u32(&s_diagnostics.read_driver_error);
            if (out != NULL) {
                out->line_error = true;
            }
        }
    }
}

void ld2450_uart_get_diagnostics(ld2450_uart_diagnostics_t *out)
{
    if (out != NULL) {
        *out = s_diagnostics;
    }
}
