idf_component_register(
    SRCS "habit_rule_engine.c" "rule_loader.c" "time_provider.c"
    INCLUDE_DIRS "include"
    REQUIRES app_config esp_timer freertos json log
)
