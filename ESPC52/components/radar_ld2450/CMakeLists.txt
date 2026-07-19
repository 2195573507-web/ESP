idf_component_register(
    SRCS
        "ld2450_parser.c"
        "radar_state_codec.c"
        "radar_service.c"
    INCLUDE_DIRS "include"
    REQUIRES freertos log
)
