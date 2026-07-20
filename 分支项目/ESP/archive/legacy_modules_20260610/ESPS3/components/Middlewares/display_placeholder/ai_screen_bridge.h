#ifndef AI_SCREEN_BRIDGE_H
#define AI_SCREEN_BRIDGE_H

#include "esp_err.h"

/**
 * @file ai_screen_bridge.h
 * @brief Screen command 执行桥接层。
 *
 * 调用方法：Screen 不直接请求服务器或模型，只执行上层传来的显示命令。当前底层屏幕未接入时
 * 先打印日志，保证后续显示命令能有稳定接口。
 */

typedef enum {
    AI_SCREEN_CMD_CLEAR = 0,      // 清屏。
    AI_SCREEN_CMD_SHOW_TEXT,      // 显示普通文本。
    AI_SCREEN_CMD_SHOW_STATUS,    // 显示系统状态。
    AI_SCREEN_CMD_SHOW_SENSOR,    // 显示传感器摘要。
    AI_SCREEN_CMD_SHOW_VOICE_TEXT,// 显示语音链路文本。
} ai_screen_cmd_type_t;

typedef struct {
    ai_screen_cmd_type_t type; // 命令类型。
    const char *title;         // 标题，可为空。
    const char *text;          // 文本，可为空。
    int timeout_ms;            // 显示超时，<=0 表示由底层决定。
} ai_screen_command_t;

/** 调用方法：screen_service_init() 内调用；当前底层屏幕未接入时仍返回 ESP_OK。 */
esp_err_t ai_screen_bridge_init(void);

/** 调用方法：上层传入完整显示命令结构；cmd 不能为空。 */
esp_err_t ai_screen_bridge_execute(const ai_screen_command_t *cmd);

/** 调用方法：显示普通文本时调用；title/text 可为空，timeout_ms <= 0 表示默认时长。 */
esp_err_t ai_screen_bridge_show_text(const char *title,
                                     const char *text,
                                     int timeout_ms);

/** 调用方法：语音链路需要显示识别/回复文本时调用。 */
esp_err_t ai_screen_bridge_show_voice_text(const char *text);

/** 调用方法：清除屏幕内容或重置显示状态时调用。 */
esp_err_t ai_screen_bridge_clear(void);

#endif // AI_SCREEN_BRIDGE_H
