idf_component_register(
                    SRCS "main.c"
                    INCLUDE_DIRS "."
                    REQUIRES Middlewares app_config freertos log
)
