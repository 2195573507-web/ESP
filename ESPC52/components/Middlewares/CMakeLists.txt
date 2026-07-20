set(wifi_srcs
            wifi/wifi_manager.c)

set(app_orchestrator_srcs
            app_orchestrator/app_orchestrator.c)

set(mic_srcs
            mic/mic_adc_test.c
            mic/mic_adc_pcm.c
            mic/mic_vad.c)

set(server_voice_srcs
            server_voice/server_voice_client.c)

set(wake_srcs
            wake/local_wake_word.c
            wake/wake_prompt_cache.c)

set(screen_srcs
            display_placeholder/ai_screen_bridge.c
            display_placeholder/screen_service.c
            display_placeholder/screen_server_client.c)

set(speaker_srcs
            speaker/speaker_player.c
            speaker/speaker_resample.c)

set(voice_srcs
            voice_domain/voice_chain.c)

set(home_ai_srcs
            home_ai/c5_voice_session_client.c)

set(runtime_srcs
            runtime/app_runtime.c
            runtime/c5_backpressure_controller.c
            runtime/c5_event_bus.c
            runtime/c5_resource_manager.c
            runtime/c5_runtime_workers.c)

set(memory_srcs
            memory/c5_memory.c)

set(device_protocol_srcs
            device_protocol/device_protocol_metadata.c
            device_protocol/device_stream_client.c)

set(sensor_srcs
            sensor_domain/bme690/driver/bme690.c
            sensor_domain/bme690/service/bme_air_quality.c
            sensor_domain/bme690/service/bme_sensor_service.c
            sensor_domain/bme690/server_client/bme_server_client.c
            )
set(radar_ble_srcs
            radar_ble/radar_ble_transport.c
            radar_ble/radar_ble_runtime.c)

set(radar_domain_srcs
            radar_domain/radar_memory_manager.c
            radar_domain/radar_buffer.c
            radar_domain/radar_upload_queue.c
            radar_domain/radar_resource_adapter.c
            radar_domain/radar_worker.c)

set(system_srcs
            command_domain/system_command/system_service.c
            command_domain/system_command/system_server_client.c)

set(srcs
            ${wifi_srcs}
            ${app_orchestrator_srcs}
            ${mic_srcs}
            ${server_voice_srcs}
            ${wake_srcs}
            ${screen_srcs}
            ${speaker_srcs}
            ${voice_srcs}
            ${home_ai_srcs}
            ${runtime_srcs}
            ${memory_srcs}
            ${device_protocol_srcs}
            ${sensor_srcs}
            ${radar_ble_srcs}
            ${radar_domain_srcs}
            ${system_srcs})

set(include_dirs
            app_orchestrator)

set(priv_include_dirs
            wifi
            mic
            server_voice
            wake
            display_placeholder
            speaker
            voice_domain
            home_ai
            runtime
            memory
            device_protocol
            sensor_domain/bme690/driver
            sensor_domain/bme690/service
            sensor_domain/bme690/server_client
            sensor_domain/radar
            radar_ble/include
            radar_domain
            command_domain/system_command)

set(requires
            esp_wifi
            esp_event
            esp_netif
            nvs_flash
            spiffs
            esp_adc
            app_config
            esp_rom
            esp_ringbuf
            esp_timer
            freertos
            heap
            esp_http_client
            lwip
            IIS
            IIC
            log
            app_time_sync
            server_comm
            esp111_protocol_common
            radar_ld2450
            radar_domain
            bt
            espressif__esp-sr)

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS ${include_dirs}
                       PRIV_INCLUDE_DIRS ${priv_include_dirs}
                       REQUIRES ${requires})
component_compile_options(-Wno-error=format -Wno-format)
