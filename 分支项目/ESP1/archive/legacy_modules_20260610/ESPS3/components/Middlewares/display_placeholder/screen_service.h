#ifndef SCREEN_SERVICE_H
#define SCREEN_SERVICE_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 调用方法：系统启动时调用一次，内部初始化 ai_screen_bridge。 */
esp_err_t screen_service_init(void);

/** 调用方法：需要在屏幕显示文本时调用；timeout_ms <= 0 表示由底层决定显示时长。 */
esp_err_t screen_service_show_text(const char *title, const char *text, int timeout_ms);

/** 调用方法：需要清空屏幕或结束一轮显示时调用。 */
esp_err_t screen_service_clear(void);

#ifdef __cplusplus
}
#endif

#endif /* SCREEN_SERVICE_H */
