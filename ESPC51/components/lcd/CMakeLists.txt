idf_component_register(
    SRCS
        "lcd_driver.c"
        "lcd_fault_injection.c"
        "lcd_touch.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_lcd
        esp_driver_spi
        esp_driver_gpio
        freertos
        heap
        log
        esp_timer
        IIC
        lvgl
        esp_lvgl_port
)
