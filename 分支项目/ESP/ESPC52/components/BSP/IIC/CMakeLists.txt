idf_component_register(
    SRCS "iic.c"
    INCLUDE_DIRS "."
    REQUIRES esp_driver_i2c esp_driver_gpio log
)

component_compile_options(-Wno-error=format -Wno-format)
