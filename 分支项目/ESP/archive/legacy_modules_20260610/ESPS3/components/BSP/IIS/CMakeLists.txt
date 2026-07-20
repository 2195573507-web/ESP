idf_component_register(SRCS "iis.c"
                       INCLUDE_DIRS "."
                       REQUIRES app_config esp_common esp_driver_gpio esp_driver_i2s
                       PRIV_REQUIRES freertos log soc)
