#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
ESPS3_ROOT=$(CDPATH= cd -- "$ROOT/../.." && pwd)
BUILD_DIR="${TMPDIR:-/tmp}/environment_alarm_host_tests.$$"
CC_BIN=${CC:-cc}
IDF_CJSON="${IDF_PATH:-/Users/zhiqin/.espressif/v5.5.4/esp-idf}/components/json/cJSON"

mkdir -p "$BUILD_DIR"

$CC_BIN -std=c11 -Wall -Wextra -Werror -DENV_ALARM_EVENT_LOG_ENABLED=0 \
    -include "$ROOT/test/host_stubs/host_compat.h" \
    -I"$ROOT/test/host_stubs" -I"$ROOT/include" -I"$ROOT/src" \
    "$ROOT/src/environment_alarm_config.c" \
    "$ROOT/src/environment_alarm_history.c" \
    "$ROOT/src/environment_alarm_events.c" \
    "$ROOT/src/environment_alarm_engine.c" \
    "$ROOT/src/environment_alarm_rules.c" \
    "$ROOT/test/test_environment_alarm_host.c" \
    -lm -o "$BUILD_DIR/environment_alarm_engine_host"
"$BUILD_DIR/environment_alarm_engine_host"

$CC_BIN -std=c11 -Wall -Wextra -Werror -DENV_ALARM_EVENT_LOG_ENABLED=0 \
    -include "$ROOT/test/host_stubs/host_compat.h" \
    -I"$ROOT/test/host_stubs" -I"$ROOT/include" -I"$ROOT/src" \
    "$ROOT/src/environment_alarm_config.c" \
    "$ROOT/src/environment_alarm_history.c" \
    "$ROOT/src/environment_alarm_events.c" \
    "$ROOT/src/environment_alarm_engine.c" \
    "$ROOT/src/environment_alarm_rules.c" \
    "$ROOT/test/test_environment_alarm_acceptance_host.c" \
    -lm -o "$BUILD_DIR/environment_alarm_acceptance_host"
"$BUILD_DIR/environment_alarm_acceptance_host"

$CC_BIN -std=c11 -Wall -Wextra -Werror \
    -I"$ESPS3_ROOT/components/Middlewares/environment_alarm_adapter" \
    "$ESPS3_ROOT/components/Middlewares/environment_alarm_adapter/environment_alarm_sequence.c" \
    "$ROOT/test/test_environment_alarm_adapter_host.c" \
    -o "$BUILD_DIR/environment_alarm_adapter_host"
"$BUILD_DIR/environment_alarm_adapter_host"

$CC_BIN -std=c11 -Wall -Wextra -Werror \
    -I"$ROOT/test/host_stubs" \
    -I"$ESPS3_ROOT/components/Middlewares/environment_alarm_reporter" \
    "$ESPS3_ROOT/components/Middlewares/environment_alarm_reporter/environment_alarm_delivery.c" \
    "$ROOT/test/test_environment_alarm_delivery_host.c" \
    -o "$BUILD_DIR/environment_alarm_delivery_host"
"$BUILD_DIR/environment_alarm_delivery_host"

$CC_BIN -std=c11 -Wall -Wextra -Werror -DENV_ALARM_EVENT_LOG_ENABLED=0 -DENV_ALARM_HOST_MUTABLE_TIME=1 \
    -include "$ROOT/test/host_stubs/host_compat.h" \
    -I"$IDF_CJSON" -I"$ROOT/test/host_stubs" -I"$ROOT/include" -I"$ROOT/src" \
    -I"$ESPS3_ROOT/components/Middlewares/environment_alarm_adapter" \
    -I"$ESPS3_ROOT/components/Middlewares/environment_alarm_reporter" \
    -I"$ESPS3_ROOT/components/Middlewares/protocol_adapter" \
    -I"$ESPS3_ROOT/components/esp111_protocol_common/include" \
    "$IDF_CJSON/cJSON.c" \
    "$ROOT/src/environment_alarm_config.c" \
    "$ROOT/src/environment_alarm_history.c" \
    "$ROOT/src/environment_alarm_events.c" \
    "$ROOT/src/environment_alarm_engine.c" \
    "$ROOT/src/environment_alarm_rules.c" \
    "$ESPS3_ROOT/components/Middlewares/environment_alarm_adapter/environment_alarm_sequence.c" \
    "$ESPS3_ROOT/components/Middlewares/environment_alarm_adapter/environment_alarm_adapter.c" \
    "$ROOT/test/host_stubs/adapter_stubs.c" \
    "$ROOT/test/test_environment_alarm_adapter_ingest_host.c" \
    -lm -o "$BUILD_DIR/environment_alarm_adapter_ingest_host"
"$BUILD_DIR/environment_alarm_adapter_ingest_host"

$CC_BIN -std=c11 -Wall -Wextra -Werror -DENV_ALARM_EVENT_LOG_ENABLED=0 \
    -include "$ROOT/test/host_stubs/host_compat.h" \
    -I"$ROOT/test/host_stubs" -I"$ROOT/include" -I"$ROOT/src" \
    -I"$ESPS3_ROOT/components/Middlewares/environment_alarm_reporter" \
    -I"$ESPS3_ROOT/components/Middlewares/network_worker" \
    -I"$ESPS3_ROOT/components/esp111_protocol_common/include" \
    "$ROOT/src/environment_alarm_config.c" \
    "$ROOT/src/environment_alarm_history.c" \
    "$ROOT/src/environment_alarm_events.c" \
    "$ROOT/src/environment_alarm_engine.c" \
    "$ROOT/src/environment_alarm_rules.c" \
    "$ESPS3_ROOT/components/Middlewares/environment_alarm_reporter/environment_alarm_delivery.c" \
    "$ESPS3_ROOT/components/Middlewares/environment_alarm_reporter/environment_alarm_reporter.c" \
    "$ROOT/test/host_stubs/reporter_stubs.c" \
    "$ROOT/test/test_environment_alarm_reporter_host.c" \
    -lm -o "$BUILD_DIR/environment_alarm_reporter_host"
"$BUILD_DIR/environment_alarm_reporter_host"

echo "environment alarm host tests: PASS"
