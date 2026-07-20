idf_component_register(
    SRCS
        "server_comm_config.c"
        "gateway_link.c"
        "server_comm_http.c"
        "../terminal_config/terminal_config.c"
    INCLUDE_DIRS "." "../terminal_config"
    REQUIRES esp111_protocol_common esp_http_client esp_timer esp_wifi heap log nvs_flash
)

component_compile_options(-Wno-error=format -Wno-format)
