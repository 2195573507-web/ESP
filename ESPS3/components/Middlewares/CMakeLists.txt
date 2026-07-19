set(gateway_srcs
            gateway_config/gateway_config.c
            gateway_wifi/gateway_wifi.c
            network_worker/network_worker.c
            network_replay_worker/network_replay_worker.c
            bme_cache_manager/bme_cache_manager.c
            child_registry/child_registry.c
            resource_manager/resource_manager.c
            protocol_adapter/protocol_adapter.c
            server_client/server_client.c
            gateway_event_reporter/gateway_event_reporter.c
            sensor_aggregator/sensor_aggregator.c
            smart_home_gateway/smart_home_gateway.c
            voice_proxy/voice_proxy.c
            wake_prompt_cache_gateway/wake_prompt_cache_gateway.c
            command_router/command_router.c
            offline_policy/offline_policy.c
            runtime/s3_event_bus.c
            runtime/s3_scheduler.c
            radar_domain/radar_protocol.c
            radar_domain/radar_registry.c
            radar_domain/radar_remote_ingest.c
            radar_domain/radar_gateway_ingest.c
            radar_domain/radar_local_adapter.c
            radar_domain/radar_rate_manager.c
            radar_domain/radar_log_manager.c
            radar_domain/radar_diagnostics.c
            radar_domain/radar_coordinate_transform.c
            radar_domain/radar_zone_map.c
            radar_domain/radar_target_tracker.c
            radar_domain/radar_spatial_state.c
            radar_ingest/radar_ingest.c
            device_stream_gateway/device_stream_gateway.c
            local_http_server/radar_local_handler.c
            local_http_server/local_http_server.c
            gateway_orchestrator/gateway_orchestrator.c)

set(include_dirs
            gateway_orchestrator)

set(priv_include_dirs
            gateway_config
            gateway_wifi
            network_worker
            network_replay_worker
            bme_cache_manager
            child_registry
            resource_manager
            protocol_adapter
            server_client
            gateway_event_reporter
            sensor_aggregator
            smart_home_gateway
            voice_proxy
            wake_prompt_cache_gateway
            command_router
            offline_policy
            runtime
            radar_ingest
            radar_domain/include
            device_stream_gateway
            local_http_server)

set(requires
            app_config
            esp_event
            esp_hw_support
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
            radar_ld2450
            spiffs
            spi_flash)

idf_component_register(SRCS ${gateway_srcs}
                       INCLUDE_DIRS ${include_dirs}
                       PRIV_INCLUDE_DIRS ${priv_include_dirs}
                       REQUIRES ${requires})

component_compile_options(-Wno-error=format -Wno-format)
