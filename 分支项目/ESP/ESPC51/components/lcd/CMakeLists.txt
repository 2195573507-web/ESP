idf_component_register(
    SRCS "lcd.c" "lcd_lvgl_service.c" "touch/touch_cst816t.c"
    INCLUDE_DIRS "." "touch"
    REQUIRES driver esp_lcd IIC ui
)
