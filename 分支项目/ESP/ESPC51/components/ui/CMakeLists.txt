idf_component_register(
    SRCS "boot_screen.c"
    INCLUDE_DIRS "."
    REQUIRES lvgl__lvgl espressif__esp_lvgl_port log
)
