#include "ld2450_config.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ld2450_uart.h"

/*
 * LD2450 配置命令在数据帧流中返回确认帧。本模块负责边界匹配、命令发送及
 * 超时后的数据流恢复；不保存任何业务状态，避免配置过程影响存在状态机。
 */

static const uint8_t s_config_header[4] = {0xFD, 0xFC, 0xFB, 0xFA};
static const uint8_t s_config_tail[4] = {0x04, 0x03, 0x02, 0x01};
static const uint8_t s_data_header[4] = {0xAA, 0xFF, 0x03, 0x00};

size_t ld2450_config_build_command(uint16_t command,
                                   const uint8_t *value,
                                   size_t value_len,
                                   uint8_t *out,
                                   size_t out_size)
{
    if ((value == NULL && value_len > 0U) || out == NULL || value_len > UINT16_MAX - 2U) {
        return 0U;
    }

    const uint16_t payload_len = (uint16_t)(2U + value_len);
    const size_t total_len = 4U + 2U + payload_len + 4U;
    if (out_size < total_len) {
        return 0U;
    }

    memcpy(out, s_config_header, sizeof(s_config_header));
    out[4] = (uint8_t)(payload_len & 0xFFU);
    out[5] = (uint8_t)(payload_len >> 8);
    out[6] = (uint8_t)(command & 0xFFU);
    out[7] = (uint8_t)(command >> 8);
    if (value_len > 0U) {
        memcpy(&out[8], value, value_len);
    }
    memcpy(&out[8U + value_len], s_config_tail, sizeof(s_config_tail));
    return total_len;
}

bool ld2450_config_ack_matches(const uint8_t *data,
                               size_t data_len,
                               uint16_t command,
                               const uint8_t **out_value,
                               size_t *out_value_len)
{
    if (out_value != NULL) {
        *out_value = NULL;
    }
    if (out_value_len != NULL) {
        *out_value_len = 0U;
    }
    if (data == NULL || data_len < 12U) {
        return false;
    }

    for (size_t offset = 0U; offset + 12U <= data_len; ++offset) {
        if (memcmp(&data[offset], s_config_header, sizeof(s_config_header)) != 0) {
            continue;
        }
        const uint16_t payload_len =
            (uint16_t)data[offset + 4U] | ((uint16_t)data[offset + 5U] << 8);
        const size_t total_len = 4U + 2U + payload_len + 4U;
        if (payload_len < 4U || offset + total_len > data_len) {
            continue;
        }
        if (memcmp(&data[offset + total_len - 4U], s_config_tail, sizeof(s_config_tail)) != 0) {
            continue;
        }

        const uint16_t ack_command =
            (uint16_t)data[offset + 6U] | ((uint16_t)data[offset + 7U] << 8);
        const uint16_t result =
            (uint16_t)data[offset + 8U] | ((uint16_t)data[offset + 9U] << 8);
        if (ack_command != (uint16_t)(command | 0x0100U) || result != 0U) {
            continue;
        }
        if (out_value != NULL) {
            *out_value = &data[offset + 10U];
        }
        if (out_value_len != NULL) {
            *out_value_len = payload_len - 4U;
        }
        return true;
    }
    return false;
}

static esp_err_t send_command_and_wait(uint16_t command,
                                       const uint8_t *value,
                                       size_t value_len,
                                       uint32_t timeout_ms)
{
    uint8_t frame[64];
    size_t frame_len =
        ld2450_config_build_command(command, value, value_len, frame, sizeof(frame));
    if (frame_len == 0U) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (ld2450_uart_write(frame, frame_len) != (int)frame_len) {
        return ESP_FAIL;
    }

    uint8_t response[192];
    size_t used = 0U;
    uint32_t waited_ms = 0U;
    while (waited_ms < timeout_ms) {
        if (used == sizeof(response)) {
            memmove(response, response + sizeof(response) / 2U, sizeof(response) / 2U);
            used = sizeof(response) / 2U;
        }
        const uint32_t slice_ms = timeout_ms - waited_ms > 20U ? 20U : timeout_ms - waited_ms;
        int read_len = ld2450_uart_read(response + used, sizeof(response) - used, slice_ms);
        waited_ms += slice_ms;
        if (read_len < 0) {
            return ESP_FAIL;
        }
        used += (size_t)read_len;
        if (ld2450_config_ack_matches(response, used, command, NULL, NULL)) {
            return ESP_OK;
        }
    }

    return ESP_ERR_TIMEOUT;
}

static esp_err_t wait_for_data_resume(uint32_t timeout_ms)
{
    uint8_t window[4] = {0};
    size_t used = 0U;
    uint32_t waited_ms = 0U;

    while (waited_ms < timeout_ms) {
        uint8_t byte = 0U;
        const uint32_t slice_ms = timeout_ms - waited_ms > 20U ? 20U : timeout_ms - waited_ms;
        int read_len = ld2450_uart_read(&byte, 1U, slice_ms);
        waited_ms += slice_ms;
        if (read_len < 0) {
            return ESP_FAIL;
        }
        if (read_len == 0) {
            continue;
        }
        if (used < sizeof(window)) {
            window[used++] = byte;
        } else {
            memmove(window, window + 1, sizeof(window) - 1U);
            window[sizeof(window) - 1U] = byte;
        }
        if (used == sizeof(window) && memcmp(window, s_data_header, sizeof(window)) == 0) {
            return ESP_OK;
        }
    }

    return ESP_ERR_TIMEOUT;
}

esp_err_t ld2450_config_execute(uint16_t command,
                                const uint8_t *value,
                                size_t value_len,
                                uint32_t timeout_ms)
{
    if (!ld2450_uart_is_enabled()) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (command == LD2450_CONFIG_COMMAND_ENABLE ||
        command == LD2450_CONFIG_COMMAND_DISABLE ||
        timeout_ms == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    static const uint8_t enable_value[2] = {0x01, 0x00};
    esp_err_t ret = send_command_and_wait(LD2450_CONFIG_COMMAND_ENABLE,
                                          enable_value,
                                          sizeof(enable_value),
                                          timeout_ms);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = send_command_and_wait(command, value, value_len, timeout_ms);
    esp_err_t end_ret =
        send_command_and_wait(LD2450_CONFIG_COMMAND_DISABLE, NULL, 0U, timeout_ms);
    if (ret != ESP_OK) {
        return ret;
    }
    if (end_ret != ESP_OK) {
        return end_ret;
    }
    return wait_for_data_resume(timeout_ms);
}
