idf_component_register(
    SRCS
        "server_comm_config.c"
        "server_comm_http.c"
    INCLUDE_DIRS "."
    REQUIRES esp_http_client esp_wifi heap log
)

component_compile_options(-Wno-error=format -Wno-format)
