/**
 * @file screen_service.c
 * @brief C5 终端 display placeholder 服务门面。
 *
 * 本文件属于 ESP32-C5 终端（ESPC51/ESPC52 共用），只把 system command/display
 * 调用转发到 ai_screen_bridge，验证上层接口可用。本文件不接真实 LCD 驱动、不轮询
 * Server，也不保存显示状态。
 */

#include "screen_service.h"

#include "ai_screen_bridge.h"

esp_err_t screen_service_init(void)
{
    return ai_screen_bridge_init();
}

esp_err_t screen_service_show_text(const char *title, const char *text, int timeout_ms)
{
    return ai_screen_bridge_show_text(title, text, timeout_ms);
}

esp_err_t screen_service_clear(void)
{
    return ai_screen_bridge_clear();
}
