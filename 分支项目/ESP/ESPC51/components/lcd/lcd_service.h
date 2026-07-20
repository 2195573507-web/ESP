#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    LCD_DASHBOARD_AIR_INIT = 0,
    LCD_DASHBOARD_AIR_READY,
    LCD_DASHBOARD_AIR_DEGRADED,
    LCD_DASHBOARD_AIR_CALIBRATING,
} lcd_dashboard_air_state_t;

typedef enum {
    LCD_DASHBOARD_MOTION_INIT = 0,
    LCD_DASHBOARD_MOTION_IDLE,
    LCD_DASHBOARD_MOTION_MOTION,
} lcd_dashboard_motion_state_t;

typedef enum {
    LCD_DASHBOARD_VOICE_LISTEN = 0,
    LCD_DASHBOARD_VOICE_WAKE,
    LCD_DASHBOARD_VOICE_REC,
    LCD_DASHBOARD_VOICE_WAIT,
    LCD_DASHBOARD_VOICE_PLAY,
    LCD_DASHBOARD_VOICE_ERR,
} lcd_dashboard_voice_state_t;

typedef struct {
    bool bme_valid;
    float temperature_c;
    float humidity_percent;
    float pressure_hpa;
    uint32_t gas_resistance_ohm;
    bool gas_valid;
    lcd_dashboard_air_state_t air_state;
    bool csi_valid;
    float motion_score;
    float confidence;
    lcd_dashboard_motion_state_t motion_state;
    bool network_ok;
    bool gateway_ok;
    lcd_dashboard_voice_state_t voice_state;
    /* Published audio-output edge; the LCD uses it only for the cat mouth. */
    bool speaker_active;
} lcd_dashboard_snapshot_t;

/** Called from the LVGL timer task to copy already-published state only. */
typedef bool (*lcd_dashboard_snapshot_provider_t)(lcd_dashboard_snapshot_t *out_snapshot,
                                                  void *user_ctx);

esp_err_t lcd_service_start(void);
bool lcd_service_is_started(void);
void lcd_service_set_snapshot_provider(lcd_dashboard_snapshot_provider_t provider, void *user_ctx);

#ifdef __cplusplus
}
#endif
