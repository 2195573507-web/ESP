#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
OUT=$(mktemp "${TMPDIR:-/tmp}/c5-radar-stream.XXXXXX")
trap 'rm -f "$OUT"' EXIT

cc -std=c11 -Wall -Wextra -Werror -DRADAR_BLE_HOST_TEST \
  -I"$ROOT/ESPC51/components/Middlewares/radar_ble/include" \
  "$ROOT/tools/test_c5_radar_ble_stream.c" \
  "$ROOT/ESPC51/components/Middlewares/radar_ble/radar_ble_stream.c" \
  -o "$OUT"
"$OUT"
echo "C5 BLE stream host test: PASS"
