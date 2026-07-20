/**
 * @file ai_screen_bridge.c
 * @brief C5 终端 display placeholder 执行桥。
 *
 * 本文件属于 ESP32-C5 终端（ESPC51/ESPC52 共用），当前只验证上层 display/lcd
 * 命令能按稳定接口传入并记录日志。本文件不驱动真实 LCD、不维护显示缓冲，也不向
 * Server 拉取显示内容。
 */

#include "ai_screen_bridge.h"

#include "app_debug_config.h"
#include "esp_log.h"

static const char *TAG = "ai_screen_bridge";

esp_err_t ai_screen_bridge_init(void)
{
    if (APP_DEBUG_SCREEN_BRIDGE) {
        ESP_LOGI(TAG, "screen bridge reserved");
    }
    return ESP_OK;
}

esp_err_t ai_screen_bridge_execute(const ai_screen_command_t *cmd)
{
    if (cmd == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (APP_DEBUG_SCREEN_BRIDGE) {
        ESP_LOGI(TAG,
                 "screen cmd type=%d title=%s text=%s timeout_ms=%d",
                 (int)cmd->type,
                 cmd->title != NULL ? cmd->title : "",
                 cmd->text != NULL ? cmd->text : "",
                 cmd->timeout_ms);
    }
    return ESP_OK;
}

esp_err_t ai_screen_bridge_show_text(const char *title,
                                     const char *text,
                                     int timeout_ms)
{
    ai_screen_command_t cmd = {
        .type = AI_SCREEN_CMD_SHOW_TEXT,
        .title = title,
        .text = text,
        .timeout_ms = timeout_ms,
    };
    return ai_screen_bridge_execute(&cmd);
}

esp_err_t ai_screen_bridge_show_voice_text(const char *text)
{
    ai_screen_command_t cmd = {
        .type = AI_SCREEN_CMD_SHOW_VOICE_TEXT,
        .title = "语音",
        .text = text,
        .timeout_ms = 5000,
    };
    return ai_screen_bridge_execute(&cmd);
}

esp_err_t ai_screen_bridge_clear(void)
{
    ai_screen_command_t cmd = {
        .type = AI_SCREEN_CMD_CLEAR,
    };
    return ai_screen_bridge_execute(&cmd);
}
