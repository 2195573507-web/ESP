#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
DOMAIN_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
PROJECT_DIR=$(CDPATH= cd -- "$DOMAIN_DIR/../../.." && pwd)
IDF_ROOT=${IDF_PATH:-/Users/zhiqin/.espressif/v5.5.4/esp-idf}
OUTPUT_DIR="${TMPDIR:-/tmp}/esps3-radar-domain-host-tests"
mkdir -p "$OUTPUT_DIR"
OUTPUT="$OUTPUT_DIR/radar_ingest_host_tests"
SPATIAL_OUTPUT="$OUTPUT_DIR/radar_spatial_host_tests"
GATEWAY_OUTPUT="$OUTPUT_DIR/radar_gateway_host_tests"
ADAPTER_OUTPUT="$OUTPUT_DIR/radar_adapter_host_tests"
PERSON_OUTPUT="$OUTPUT_DIR/radar_person_continuity_host_tests"

cc -std=c11 -Wall -Wextra -Werror \
  -I"$DOMAIN_DIR/include" \
  -I"$PROJECT_DIR/components/radar_ld2450/include" \
  "$DOMAIN_DIR/radar_person_continuity.c" \
  "$SCRIPT_DIR/test_radar_person_continuity.c" \
  -lm -o "$PERSON_OUTPUT"

"$PERSON_OUTPUT"

cc -std=c11 -Wall -Wextra -Werror -DRADAR_DOMAIN_HOST_TEST \
  -I"$DOMAIN_DIR/include" \
  -I"$PROJECT_DIR/components/radar_ld2450/include" \
  -I"$IDF_ROOT/components/json/cJSON" \
  "$DOMAIN_DIR/radar_protocol.c" \
  "$DOMAIN_DIR/radar_registry.c" \
  "$DOMAIN_DIR/radar_remote_ingest.c" \
  "$IDF_ROOT/components/json/cJSON/cJSON.c" \
  "$SCRIPT_DIR/test_radar_ingest.c" \
  -lm -o "$OUTPUT"

"$OUTPUT"

cc -std=c11 -Wall -Wextra -Werror \
  -I"$DOMAIN_DIR/include" \
  -I"$PROJECT_DIR/components/radar_ld2450/include" \
  "$DOMAIN_DIR/radar_coordinate_transform.c" \
  "$DOMAIN_DIR/radar_zone_map.c" \
  "$DOMAIN_DIR/radar_target_tracker.c" \
  "$DOMAIN_DIR/radar_person_continuity.c" \
  "$DOMAIN_DIR/radar_rate_manager.c" \
  "$DOMAIN_DIR/radar_spatial_state.c" \
  "$PROJECT_DIR/components/radar_ld2450/radar_uart_recovery.c" \
  "$SCRIPT_DIR/test_radar_spatial.c" \
  -lm -o "$SPATIAL_OUTPUT"

"$SPATIAL_OUTPUT"

cc -std=c11 -Wall -Wextra -Werror -DRADAR_DOMAIN_HOST_TEST -DRADAR_GATEWAY_HOST_TEST \
  -I"$DOMAIN_DIR/include" \
  -I"$PROJECT_DIR/components/radar_ld2450/include" \
  -I"$IDF_ROOT/components/json/cJSON" \
  "$DOMAIN_DIR/radar_gateway_ingest.c" \
  "$DOMAIN_DIR/radar_registry.c" \
  "$DOMAIN_DIR/radar_protocol.c" \
  "$DOMAIN_DIR/radar_coordinate_transform.c" \
  "$DOMAIN_DIR/radar_zone_map.c" \
  "$DOMAIN_DIR/radar_target_tracker.c" \
  "$DOMAIN_DIR/radar_person_continuity.c" \
  "$DOMAIN_DIR/radar_rate_manager.c" \
  "$DOMAIN_DIR/radar_spatial_state.c" \
  "$PROJECT_DIR/components/radar_ld2450/radar_uart_recovery.c" \
  "$IDF_ROOT/components/json/cJSON/cJSON.c" \
  "$SCRIPT_DIR/test_radar_gateway_ingest.c" \
  -lm -o "$GATEWAY_OUTPUT"

"$GATEWAY_OUTPUT"

cc -std=c11 -Wall -Wextra -Werror -DRADAR_DOMAIN_HOST_TEST -DRADAR_GATEWAY_HOST_TEST -DRADAR_INGEST_HOST_TEST \
  -I"$PROJECT_DIR/components/Middlewares/radar_ingest" \
  -I"$DOMAIN_DIR/include" \
  -I"$PROJECT_DIR/components/radar_ld2450/include" \
  -I"$IDF_ROOT/components/json/cJSON" \
  "$PROJECT_DIR/components/Middlewares/radar_ingest/radar_ingest.c" \
  "$DOMAIN_DIR/radar_gateway_ingest.c" \
  "$DOMAIN_DIR/radar_registry.c" \
  "$DOMAIN_DIR/radar_protocol.c" \
  "$DOMAIN_DIR/radar_coordinate_transform.c" \
  "$DOMAIN_DIR/radar_zone_map.c" \
  "$DOMAIN_DIR/radar_target_tracker.c" \
  "$DOMAIN_DIR/radar_person_continuity.c" \
  "$DOMAIN_DIR/radar_rate_manager.c" \
  "$DOMAIN_DIR/radar_spatial_state.c" \
  "$PROJECT_DIR/components/radar_ld2450/radar_uart_recovery.c" \
  "$IDF_ROOT/components/json/cJSON/cJSON.c" \
  "$PROJECT_DIR/components/Middlewares/radar_ingest/tests/test_radar_ingest.c" \
  -lm -o "$ADAPTER_OUTPUT"

"$ADAPTER_OUTPUT"
