/**
 * @file screen_service.c
 * @brief Bounded display-command mailbox with a generation-scoped LCD adapter.
 */

#include "screen_service.h"

#include <string.h>

#include "ai_screen_bridge.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"

#define SCREEN_SERVICE_DEFAULT_TTL_MS 5000

typedef struct {
    bool initialized;
    uint32_t next_generation;
    screen_service_command_t latest;
    screen_service_lcd_adapter_t adapter;
    uint32_t adapter_generation;
} screen_service_context_t;

static screen_service_context_t s_screen;
static portMUX_TYPE s_screen_lock = portMUX_INITIALIZER_UNLOCKED;

static int64_t screen_service_now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static void screen_service_copy_command(screen_service_command_t *out)
{
    if (out == NULL) {
        return;
    }
    portENTER_CRITICAL(&s_screen_lock);
    *out = s_screen.latest;
    portEXIT_CRITICAL(&s_screen_lock);
}

static esp_err_t screen_service_submit_latest(void)
{
    screen_service_lcd_adapter_t adapter = {0};
    screen_service_command_t command = {0};
    portENTER_CRITICAL(&s_screen_lock);
    adapter = s_screen.adapter;
    command = s_screen.latest;
    portEXIT_CRITICAL(&s_screen_lock);

    if (adapter.submit == NULL) {
        return ESP_ERR_NOT_FINISHED;
    }
    if (!command.valid) {
        return ESP_OK;
    }
    return adapter.submit(adapter.context, &command);
}

esp_err_t screen_service_init(void)
{
    bool initialize_bridge = false;
    portENTER_CRITICAL(&s_screen_lock);
    if (!s_screen.initialized) {
        memset(&s_screen, 0, sizeof(s_screen));
        s_screen.initialized = true;
        initialize_bridge = true;
    }
    portEXIT_CRITICAL(&s_screen_lock);
    return initialize_bridge ? ai_screen_bridge_init() : ESP_OK;
}

esp_err_t screen_service_attach_lcd(const screen_service_lcd_adapter_t *adapter)
{
    if (adapter == NULL || adapter->submit == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    const esp_err_t init_ret = screen_service_init();
    if (init_ret != ESP_OK) {
        return init_ret;
    }

    portENTER_CRITICAL(&s_screen_lock);
    s_screen.next_generation++;
    if (s_screen.next_generation == 0U) {
        s_screen.next_generation = 1U;
    }
    s_screen.adapter = *adapter;
    s_screen.adapter_generation = s_screen.next_generation;
    portEXIT_CRITICAL(&s_screen_lock);
    return screen_service_submit_latest();
}

uint32_t screen_service_lcd_generation(void)
{
    uint32_t generation;
    portENTER_CRITICAL(&s_screen_lock);
    generation = s_screen.adapter_generation;
    portEXIT_CRITICAL(&s_screen_lock);
    return generation;
}

void screen_service_detach_lcd(uint32_t generation)
{
    portENTER_CRITICAL(&s_screen_lock);
    if (generation != 0U && generation == s_screen.adapter_generation) {
        memset(&s_screen.adapter, 0, sizeof(s_screen.adapter));
        s_screen.adapter_generation = 0U;
    }
    portEXIT_CRITICAL(&s_screen_lock);
}

void screen_service_get_latest(screen_service_command_t *out_command)
{
    screen_service_copy_command(out_command);
}

esp_err_t screen_service_show_text(const char *title, const char *text, int timeout_ms)
{
    if (text == NULL || text[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (strnlen(text, SCREEN_SERVICE_TEXT_MAX) >= SCREEN_SERVICE_TEXT_MAX ||
        (title != NULL && strnlen(title, SCREEN_SERVICE_TITLE_MAX) >= SCREEN_SERVICE_TITLE_MAX)) {
        return ESP_ERR_INVALID_SIZE;
    }
    const int64_t now_ms = screen_service_now_ms();
    const int64_t ttl_ms = timeout_ms > 0 ? timeout_ms : SCREEN_SERVICE_DEFAULT_TTL_MS;

    (void)screen_service_init();
    portENTER_CRITICAL(&s_screen_lock);
    s_screen.next_generation++;
    if (s_screen.next_generation == 0U) {
        s_screen.next_generation = 1U;
    }
    memset(&s_screen.latest, 0, sizeof(s_screen.latest));
    s_screen.latest.generation = s_screen.next_generation;
    s_screen.latest.expires_at_ms = now_ms + ttl_ms;
    s_screen.latest.valid = true;
    strlcpy(s_screen.latest.title, title != NULL ? title : "", sizeof(s_screen.latest.title));
    strlcpy(s_screen.latest.text, text, sizeof(s_screen.latest.text));
    portEXIT_CRITICAL(&s_screen_lock);

    (void)ai_screen_bridge_show_text(title, text, timeout_ms);
    return screen_service_submit_latest();
}

esp_err_t screen_service_clear(void)
{
    (void)screen_service_init();
    portENTER_CRITICAL(&s_screen_lock);
    s_screen.next_generation++;
    if (s_screen.next_generation == 0U) {
        s_screen.next_generation = 1U;
    }
    memset(&s_screen.latest, 0, sizeof(s_screen.latest));
    s_screen.latest.generation = s_screen.next_generation;
    s_screen.latest.valid = true;
    s_screen.latest.clear = true;
    portEXIT_CRITICAL(&s_screen_lock);

    (void)ai_screen_bridge_clear();
    return screen_service_submit_latest();
}
