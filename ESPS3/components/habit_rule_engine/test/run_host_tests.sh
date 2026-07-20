#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
BUILD_DIR="${TMPDIR:-/tmp}/habit_rule_engine_host_tests.$$"
IDF_CJSON="${IDF_PATH:-/Users/zhiqin/.espressif/v5.5.4/esp-idf}/components/json/cJSON"
mkdir -p "$BUILD_DIR"

${CC:-cc} -std=c11 -Wall -Wextra -Werror \
    -I"$ROOT/test/host_stubs" -I"$ROOT/include" -I"$IDF_CJSON" \
    "$IDF_CJSON/cJSON.c" \
    "$ROOT/habit_rule_engine.c" "$ROOT/rule_loader.c" "$ROOT/time_provider.c" \
    "$ROOT/test/test_habit_rule_engine.c" \
    -o "$BUILD_DIR/habit_rule_engine_host"
"$BUILD_DIR/habit_rule_engine_host"
