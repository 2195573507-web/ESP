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

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 初始化 display placeholder；system_service_init() 调用，返回 ai_screen_bridge_init() 结果。 */
esp_err_t screen_service_init(void);

/**
 * @brief 显示文本命令入口。
 *
 * 调用位置：system_server_client 执行 lcd.show_text/display.show_text 命令时。
 * @param title 标题，可为空。
 * @param text 文本，可为空。
 * @param timeout_ms 显示时长，<=0 表示默认。
 * @return ESP_OK 表示占位层接受命令；参数/内部错误按 ai_screen_bridge 返回。
 * 失败处理：system command ack 将错误回传给 S3。
 */
esp_err_t screen_service_show_text(const char *title, const char *text, int timeout_ms);

/** @brief 清屏命令入口；当前只经过占位桥，不操作真实 LCD。 */
esp_err_t screen_service_clear(void);

#ifdef __cplusplus
}
#endif

#endif /* SCREEN_SERVICE_H */
