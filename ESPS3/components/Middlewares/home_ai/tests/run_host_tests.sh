#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_dir="$root/tests"
binary="${TMPDIR:-/tmp}/home-ai-voice-session-host-test"

sh "$test_dir/test_network_recovery_contract.sh"

${CC:-cc} -std=c11 -Wall -Wextra -Werror \
    -I"$test_dir/stubs" \
    -I"$root" \
    "$root/home_ai_voice_session.c" \
    "$test_dir/test_home_ai_voice_session.c" \
    -o "$binary"

"$binary"

voice_router_binary="${TMPDIR:-/tmp}/home-ai-voice-router-host-test"

${CC:-cc} -std=c11 -Wall -Wextra -Werror \
    -I"$test_dir/stubs" \
    -I"$root" \
    -I"$root/../command_router" \
    -I"$root/../radar_domain/include" \
    -I"$root/../../radar_ld2450/include" \
    "$root/home_ai_voice_router.c" \
    "$test_dir/test_home_ai_voice_router.c" \
    -o "$voice_router_binary"

"$voice_router_binary"

emergency_coordinator_binary="${TMPDIR:-/tmp}/home-ai-emergency-coordinator-host-test"

${CC:-cc} -std=c11 -Wall -Wextra -Werror -DHOME_AI_EMERGENCY_HOST_TEST \
    -I"$test_dir/stubs" \
    -I"$root" \
    -I"$root/../environment_alarm_reporter" \
    -I"$root/../../environment_alarm_engine/include" \
    -I"$root/../radar_domain/include" \
    -I"$root/../../radar_ld2450/include" \
    "$root/home_ai_emergency_coordinator.c" \
    "$test_dir/test_home_ai_emergency_coordinator.c" \
    -o "$emergency_coordinator_binary"

"$emergency_coordinator_binary"

local_voice_command_binary="${TMPDIR:-/tmp}/home-ai-local-voice-command-host-test"

${CC:-cc} -std=c11 -Wall -Wextra -Werror -DHOME_AI_RULE_ENGINE_HOST_TEST \
    -I"$test_dir/stubs" \
    -I"$root" \
    -I"$root/../radar_domain/include" \
    -I"$root/../../radar_ld2450/include" \
    "$root/home_ai_user_override.c" \
    "$root/home_ai_virtual_device.c" \
    "$root/home_ai_local_voice_command.c" \
    "$test_dir/test_home_ai_local_voice_command.c" \
    -o "$local_voice_command_binary"

"$local_voice_command_binary"

command_ack_policy_binary="${TMPDIR:-/tmp}/command-ack-policy-host-test"

${CC:-cc} -std=c11 -Wall -Wextra -Werror \
    -I"$root/../command_router" \
    "$root/../command_router/command_ack_policy.c" \
    "$test_dir/test_command_ack_policy.c" \
    -o "$command_ack_policy_binary"

"$command_ack_policy_binary"

weather_context_binary="${TMPDIR:-/tmp}/home-ai-weather-context-host-test"
idf_root=${IDF_PATH:-/Users/zhiqin/.espressif/v5.5.4/esp-idf}

${CC:-cc} -std=c11 -Wall -Wextra -Werror \
    -I"$root" \
    -I"$root/../../esp111_protocol_common/include" \
    -I"$idf_root/components/json/cJSON" \
    "$root/home_ai_weather_context.c" \
    "$idf_root/components/json/cJSON/cJSON.c" \
    "$test_dir/test_home_ai_weather_context.c" \
    -o "$weather_context_binary"

"$weather_context_binary"

config_store_binary="${TMPDIR:-/tmp}/home-ai-config-store-host-test"

${CC:-cc} -std=c11 -Wall -Wextra -Werror -DHOME_AI_CONFIG_STORE_HOST_TEST \
    -I"$root" \
    -I"$root/../radar_domain/include" \
    -I"$root/../../radar_ld2450/include" \
    "$root/home_ai_config_store.c" \
    "$test_dir/test_home_ai_config_store.c" \
    -o "$config_store_binary"

"$config_store_binary"

event_reporter_binary="${TMPDIR:-/tmp}/home-ai-event-reporter-host-test"

${CC:-cc} -std=c11 -Wall -Wextra -Werror -ffunction-sections -fdata-sections \
    -I"$test_dir/stubs" \
    -I"$root" \
    -I"$root/../../esp111_protocol_common/include" \
    -I"$root/../radar_domain/include" \
    -I"$root/../../radar_ld2450/include" \
    -I"$idf_root/components/json/cJSON" \
    "$root/home_ai_event_reporter.c" \
    "$idf_root/components/json/cJSON/cJSON.c" \
    "$test_dir/test_home_ai_event_reporter.c" \
    -Wl,-dead_strip \
    -o "$event_reporter_binary"

"$event_reporter_binary"

room_state_binary="${TMPDIR:-/tmp}/home-ai-room-state-host-test"

${CC:-cc} -std=c11 -Wall -Wextra -Werror -DHOME_AI_ROOM_STATE_HOST_TEST \
    -I"$root" \
    -I"$root/../../esp111_protocol_common/include" \
    -I"$root/../radar_domain/include" \
    -I"$root/../../radar_ld2450/include" \
    "$root/home_ai_room_state.c" \
    "$test_dir/test_home_ai_room_state.c" \
    -o "$room_state_binary"

"$room_state_binary"

rule_engine_binary="${TMPDIR:-/tmp}/home-ai-rule-engine-host-test"

${CC:-cc} -std=c11 -Wall -Wextra -Werror \
    -DHOME_AI_RULE_ENGINE_HOST_TEST -DHOME_AI_ROOM_STATE_HOST_TEST \
    -I"$test_dir/stubs" \
    -I"$root" \
    -I"$root/../../esp111_protocol_common/include" \
    -I"$root/../radar_domain/include" \
    -I"$root/../../radar_ld2450/include" \
    -I"$idf_root/components/json/cJSON" \
    "$root/home_ai_user_override.c" \
    "$root/home_ai_rule_engine.c" \
    "$root/home_ai_room_state.c" \
    "$idf_root/components/json/cJSON/cJSON.c" \
    "$test_dir/test_home_ai_rule_engine.c" \
    -o "$rule_engine_binary"

"$rule_engine_binary"

virtual_device_binary="${TMPDIR:-/tmp}/home-ai-virtual-device-host-test"

${CC:-cc} -std=c11 -Wall -Wextra -Werror \
    -DHOME_AI_RULE_ENGINE_HOST_TEST \
    -I"$test_dir/stubs" \
    -I"$root" \
    -I"$root/../radar_domain/include" \
    -I"$root/../../radar_ld2450/include" \
    "$root/home_ai_virtual_device.c" \
    "$test_dir/test_home_ai_virtual_device.c" \
    -o "$virtual_device_binary"

"$virtual_device_binary"

history_store_binary="${TMPDIR:-/tmp}/home-ai-history-store-host-test"

${CC:-cc} -std=c11 -Wall -Wextra -Werror \
    -DHOME_AI_HISTORY_HOST_TEST \
    -I"$test_dir/stubs" \
    -I"$root" \
    "$root/home_ai_history_store.c" \
    "$test_dir/test_home_ai_history_store.c" \
    -o "$history_store_binary"

"$history_store_binary" basic
"$history_store_binary" restart
"$history_store_binary" prune
"$history_store_binary" protected
"$history_store_binary" priority
