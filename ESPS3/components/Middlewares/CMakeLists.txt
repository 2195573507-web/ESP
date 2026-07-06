set(gateway_srcs
            gateway_config/gateway_config.c
            gateway_wifi/gateway_wifi.c
            child_registry/child_registry.c
            protocol_adapter/protocol_adapter.c
            server_client/server_client.c
            gateway_event_reporter/gateway_event_reporter.c
            sensor_aggregator/sensor_aggregator.c
            smart_home_gateway/smart_home_gateway.c
            voice_proxy/voice_proxy.c
            wake_prompt_cache_gateway/wake_prompt_cache_gateway.c
            command_router/command_router.c
            offline_policy/offline_policy.c
            csi_fusion/csi_fusion.c
            csi_placeholder_gateway/csi_placeholder_gateway.c
            local_http_server/local_http_server.c
            gateway_orchestrator/gateway_orchestrator.c)

set(include_dirs
            gateway_orchestrator)

set(priv_include_dirs
            gateway_config
            gateway_wifi
            child_registry
            protocol_adapter
            server_client
            gateway_event_reporter
            sensor_aggregator
            smart_home_gateway
            voice_proxy
            wake_prompt_cache_gateway
            command_router
            offline_policy
            csi_fusion
            csi_placeholder_gateway
            local_http_server)

set(requires
            app_config
            esp_event
            esp_http_client
            esp_http_server
            esp_netif
            esp_psram
            esp_timer
            esp_wifi
            esp111_protocol_common
            freertos
            heap
            json
            log
            lwip
            nvs_flash
            spiffs
            spi_flash)

idf_component_register(SRCS ${gateway_srcs}
                       INCLUDE_DIRS ${include_dirs}
                       PRIV_INCLUDE_DIRS ${priv_include_dirs}
                       REQUIRES ${requires})

component_compile_options(-Wno-error=format -Wno-format)
