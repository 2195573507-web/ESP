idf_component_register(
    SRCS
        "server_comm_config.c"
        "gateway_link.c"
        "server_comm_http.c"
        "../terminal_config/terminal_config.c"
    INCLUDE_DIRS "." "../terminal_config"
    REQUIRES esp111_protocol_common esp_http_client esp_timer esp_wifi heap log nvs_flash
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE
    TERMINAL_CONFIG_DEFAULT_DEVICE_ID=\"sensair_shuttle_02\"
    TERMINAL_CONFIG_DEFAULT_LOCAL_ID=ESP111_PROTOCOL_LOCAL_DEVICE_ID_C52
    TERMINAL_CONFIG_DEFAULT_ALIAS=\"SensaiShuttle02\")

component_compile_options(-Wno-error=format -Wno-format)
