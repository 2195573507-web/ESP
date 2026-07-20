#pragma once

#include <stdbool.h>

typedef enum {
    LCD_FAULT_NONE = 0,
    LCD_FAULT_PANEL_IO,
    LCD_FAULT_PANEL_DRIVER,
    LCD_FAULT_LEGACY_DMA,
    LCD_FAULT_LVGL_PORT,
    LCD_FAULT_LVGL_DISPLAY,
    LCD_FAULT_SERVICE_CONTEXT,
    LCD_FAULT_WAKE_QUEUE,
    LCD_FAULT_UI_ARENA,
    LCD_FAULT_UI_TIMER,
    LCD_FAULT_TOUCH_TASK,
} lcd_fault_stage_t;

void lcd_fault_injection_arm(lcd_fault_stage_t stage);
void lcd_fault_injection_clear(void);
bool lcd_fault_injection_should_fail(lcd_fault_stage_t stage);
