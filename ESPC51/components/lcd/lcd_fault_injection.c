#include "lcd_fault_injection.h"

static lcd_fault_stage_t s_armed_stage;

void lcd_fault_injection_arm(lcd_fault_stage_t stage) { s_armed_stage = stage; }
void lcd_fault_injection_clear(void) { s_armed_stage = LCD_FAULT_NONE; }
bool lcd_fault_injection_should_fail(lcd_fault_stage_t stage)
{
    if (stage == LCD_FAULT_NONE || s_armed_stage != stage) return false;
    s_armed_stage = LCD_FAULT_NONE;
    return true;
}
