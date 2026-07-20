#ifndef AI_SCREEN_BRIDGE_H
#define AI_SCREEN_BRIDGE_H

#include "esp_err.h"

/**
 * @file ai_screen_bridge.h
 * @brief C5 终端 Screen command placeholder 执行桥。
 *
 * 本模块只验证上层显示命令接口，不接真实 LCD、不请求服务器或模型。当前底层屏幕未接入时
 * 只打印日志并返回成功/参数错误，保证 command/display 调用边界稳定。
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

/** @brief 初始化屏幕占位桥；screen_service_init() 调用，当前固定返回 ESP_OK。 */
esp_err_t ai_screen_bridge_init(void);

/**
 * @brief 执行一条显示命令占位逻辑。
 *
 * 调用位置：screen_service_show_text()/clear() 或未来显示命令入口。
 * @param cmd 显示命令，不能为空。
 * @return ESP_OK 表示上层命令形状已被接受；cmd 为空返回 ESP_ERR_INVALID_ARG。
 * 失败处理：system command ack 会把参数错误映射为命令失败；当前不涉及 LCD 硬件错误。
 */
esp_err_t ai_screen_bridge_execute(const ai_screen_command_t *cmd);

/** @brief 显示普通文本的占位入口；title/text 可为空，timeout_ms <= 0 表示默认时长。 */
esp_err_t ai_screen_bridge_show_text(const char *title,
                                     const char *text,
                                     int timeout_ms);

/** @brief 语音链路显示文本的占位入口；当前只转成显示命令并记录日志。 */
esp_err_t ai_screen_bridge_show_voice_text(const char *text);

/** @brief 清除屏幕内容的占位入口；当前不操作真实 LCD。 */
esp_err_t ai_screen_bridge_clear(void);

#endif // AI_SCREEN_BRIDGE_H
