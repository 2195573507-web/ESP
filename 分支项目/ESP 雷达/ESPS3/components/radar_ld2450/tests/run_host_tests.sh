#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
OUT="$ROOT/tests/radar_core_host_tests"

cc -std=c11 -Wall -Wextra -Werror \
  -I"$ROOT/include" \
  "$ROOT/ld2450_parser.c" \
  "$ROOT/radar_presence.c" \
  "$ROOT/radar_state_codec.c" \
  "$ROOT/tests/test_radar_core.c" \
  -o "$OUT"

"$OUT"
