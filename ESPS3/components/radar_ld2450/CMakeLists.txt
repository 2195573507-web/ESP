idf_component_register(
    SRCS
        "ld2450_parser.c"
        "ld2450_uart.c"
        "ld2450_config.c"
        "radar_presence.c"
        "radar_uart_recovery.c"
        "radar_state_codec.c"
        "radar_service.c"
    INCLUDE_DIRS "include"
    REQUIRES app_config esp_driver_uart esp_timer freertos log
)
