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
