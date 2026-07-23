#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LCD_SYSTEM_SNAPSHOT_RADAR_SOURCES 3U
#define LCD_COMMAND_TITLE_MAX 48U
#define LCD_COMMAND_TEXT_MAX 192U

typedef enum {
    LCD_WIFI_UNKNOWN = 0,
    LCD_WIFI_CONNECTING,
    LCD_WIFI_ONLINE,
    LCD_WIFI_OFFLINE,
} lcd_wifi_state_t;

typedef enum {
    LCD_VOICE_IDLE = 0,
    LCD_VOICE_WAKE,
    LCD_VOICE_RECORDING,
    LCD_VOICE_WAITING,
    LCD_VOICE_PLAYING,
    LCD_VOICE_ERROR,
} lcd_voice_state_t;

typedef struct {
    bool healthy;
    bool presence;
    bool motion;
    uint8_t person_count;
} lcd_radar_source_snapshot_t;

/* Fixed-value contract. Producers must copy values and never retain pointers. */
typedef struct {
    uint64_t timestamp_ms;
    uint32_t generation;
    lcd_wifi_state_t wifi_state;
    bool gateway_online;
    bool bme_valid;
    float temperature_c;
    float humidity_percent;
    float pressure_hpa;
    uint32_t gas_resistance_ohm;
    bool gas_valid;
    bool air_quality_valid;
    uint16_t iaq;
    lcd_radar_source_snapshot_t radar_sources[LCD_SYSTEM_SNAPSHOT_RADAR_SOURCES];
    bool room_occupied;
    bool home_occupied;
    uint8_t home_person_count;
    lcd_voice_state_t voice_state;
    bool speaker_active;
    bool wake_allowed;
    uint8_t alarm_level;
    uint32_t degraded_flags;
} lcd_system_snapshot_t;

typedef struct {
    char title[LCD_COMMAND_TITLE_MAX];
    char text[LCD_COMMAND_TEXT_MAX];
    uint32_t ttl_ms;
    uint32_t generation;
} lcd_command_t;

typedef struct {
    uint32_t generation;
    uint64_t timestamp_ms;
} lcd_wake_event_t;

esp_err_t lcd_service_start(void);
esp_err_t lcd_service_stop(void);
bool lcd_service_is_started(void);
esp_err_t lcd_service_post_snapshot(const lcd_system_snapshot_t *snapshot);
esp_err_t lcd_service_post_command(const lcd_command_t *command);

/*
 * Latches the outcome of the application startup sequence.  The UI task owns
 * the eventual Boot-to-Home transition; callers never acquire the LVGL lock.
 */
esp_err_t lcd_service_mark_boot_complete(void);

/* Called by the UI only. It posts a fixed event; it never calls voice directly. */
esp_err_t lcd_service_request_wake_event(void);
bool lcd_service_take_wake_event(lcd_wake_event_t *out_event);

#ifdef __cplusplus
}
#endif
