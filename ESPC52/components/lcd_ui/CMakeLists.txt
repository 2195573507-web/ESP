idf_component_register(
    SRCS
        "lcd_ui.c"
        "lcd_service.c"
    INCLUDE_DIRS "."
    REQUIRES
        lcd
        freertos
        heap
        log
        esp_timer
        lvgl
        esp_lvgl_port
)
