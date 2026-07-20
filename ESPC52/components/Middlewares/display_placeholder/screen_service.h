#ifndef SCREEN_SERVICE_H
#define SCREEN_SERVICE_H

/**
 * @file screen_service.h
 * @brief C5 终端 display placeholder 服务接口。
 *
 * system_service 执行 lcd.show_text/display.show_text 命令时经过本模块；当前只验证上层
 * 调用和 ack 边界，不接真实 LCD。
 */

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SCREEN_SERVICE_TITLE_MAX 64U
#define SCREEN_SERVICE_TEXT_MAX 256U

typedef struct {
    uint32_t generation;
    int64_t expires_at_ms;
    bool valid;
    bool clear;
    char title[SCREEN_SERVICE_TITLE_MAX];
    char text[SCREEN_SERVICE_TEXT_MAX];
} screen_service_command_t;

/* The adapter receives value copies only and must enqueue rather than call LVGL directly. */
typedef esp_err_t (*screen_service_submit_fn)(void *context,
                                              const screen_service_command_t *command);

typedef struct {
    screen_service_submit_fn submit;
    void *context;
} screen_service_lcd_adapter_t;

/** @brief 初始化命令邮箱和保留的调试 bridge；system_service_init() 调用。 */
esp_err_t screen_service_init(void);

/** Bind the currently running LCD generation and immediately replay the latest command. */
esp_err_t screen_service_attach_lcd(const screen_service_lcd_adapter_t *adapter);

/** Return the current adapter generation, or zero when no LCD is attached. */
uint32_t screen_service_lcd_generation(void);

/** Detach only the generation that owns the current adapter; safe to call repeatedly. */
void screen_service_detach_lcd(uint32_t generation);

/** Return a coherent value copy for LCD bootstrap or status diagnostics. */
void screen_service_get_latest(screen_service_command_t *out_command);

/**
 * @brief 显示文本命令入口。
 *
 * 调用位置：system_server_client 执行 lcd.show_text/display.show_text 命令时。
 * @param title 标题，可为空。
 * @param text 文本，可为空。
 * @param timeout_ms 显示时长，<=0 表示默认。
 * @return ESP_OK 表示已异步送入 LCD mailbox；ESP_ERR_NOT_FINISHED 表示已保存但 LCD
 * 尚未就绪；其他返回值表示命令没有写入邮箱。
 */
esp_err_t screen_service_show_text(const char *title, const char *text, int timeout_ms);

/** @brief 清屏命令入口；写入同一个 latest-only mailbox。 */
esp_err_t screen_service_clear(void);

#ifdef __cplusplus
}
#endif

#endif /* SCREEN_SERVICE_H */
