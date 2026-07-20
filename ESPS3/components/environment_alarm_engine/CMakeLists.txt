idf_component_register(
    SRCS
        "src/environment_alarm_config.c"
        "src/environment_alarm_history.c"
        "src/environment_alarm_events.c"
        "src/environment_alarm_engine.c"
        "src/environment_alarm_rules.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_timer freertos log
)
