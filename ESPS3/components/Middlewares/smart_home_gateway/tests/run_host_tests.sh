#!/bin/sh
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
test_dir="$root/tests"
idf_root=${IDF_PATH:-/Users/zhiqin/.espressif/v5.5.4/esp-idf}
binary="${TMPDIR:-/tmp}/smart-home-gateway-host-test"

${CC:-cc} -std=c11 -Wall -Wextra -Werror \
    -I"$test_dir/stubs" \
    -I"$root/../home_ai/tests/stubs" \
    -I"$root" \
    -I"$root/../home_ai" \
    -I"$root/../gateway_config" \
    -I"$root/../gateway_event_reporter" \
    -I"$root/../offline_policy" \
    -I"$root/../sensor_aggregator" \
    -I"$root/../server_client" \
    -I"$root/../protocol_adapter" \
    -I"$root/../radar_domain/include" \
    -I"$root/../../radar_ld2450/include" \
    -I"$root/../../esp111_protocol_common/include" \
    -I"$idf_root/components/json/cJSON" \
    "$root/smart_home_gateway.c" \
    "$test_dir/test_smart_home_gateway.c" \
    "$idf_root/components/json/cJSON/cJSON.c" \
    -o "$binary"

"$binary"
