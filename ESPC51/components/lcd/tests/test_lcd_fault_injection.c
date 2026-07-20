#include <assert.h>
#include "lcd_fault_injection.h"
int main(void) {
    for (int stage = LCD_FAULT_PANEL_IO; stage <= LCD_FAULT_TOUCH_TASK; ++stage) {
        lcd_fault_injection_arm((lcd_fault_stage_t)stage);
        assert(lcd_fault_injection_should_fail((lcd_fault_stage_t)stage));
        assert(!lcd_fault_injection_should_fail((lcd_fault_stage_t)stage));
    }
    lcd_fault_injection_arm(LCD_FAULT_LVGL_DISPLAY);
    lcd_fault_injection_clear();
    assert(!lcd_fault_injection_should_fail(LCD_FAULT_LVGL_DISPLAY));
    return 0;
}
