idf_component_register(
    SRCS "lcd.c" "lcd_lvgl_service.c"
    INCLUDE_DIRS "."
    REQUIRES driver esp_lcd
)
