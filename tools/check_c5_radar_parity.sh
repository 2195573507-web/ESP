#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
C51="$ROOT/ESPC51"
C52="$ROOT/ESPC52"

for path in \
  components/Middlewares/radar_ble/include/radar_ble_stream.h \
  components/Middlewares/radar_ble/include/radar_ble_transport.h \
  components/Middlewares/radar_ble/include/radar_ble_runtime.h \
  components/Middlewares/radar_ble/radar_ble_stream.c \
  components/Middlewares/radar_ble/radar_ble_transport.c \
  components/Middlewares/radar_ble/radar_ble_runtime.c \
  components/radar_ld2450/ld2450_parser.c \
  components/radar_ld2450/include/ld2450_parser.h \
  components/radar_ld2450/include/ld2450_types.h \
  components/radar_ld2450/include/radar_target_sample.h \
  components/radar_ld2450/include/radar_service.h \
  components/radar_ld2450/include/radar_state_codec.h \
  components/radar_ld2450/radar_service.c \
  components/radar_ld2450/radar_state_codec.c \
  components/Middlewares/sensor_domain/radar/radar_state_client.c \
  components/Middlewares/sensor_domain/radar/radar_state_client.h \
  components/Middlewares/radar_domain/radar_worker.c \
  components/Middlewares/radar_domain/radar_resource_adapter.c \
  components/Middlewares/radar_domain/radar_resource_adapter.h \
  components/Middlewares/sensor_domain/bme690/server_client/bme_server_client.c \
  components/Middlewares/sensor_domain/bme690/service/bme_sensor_service.c \
  components/Middlewares/runtime/c5_backpressure_controller.c; do
  cmp "$C51/$path" "$C52/$path"
done

if cmp "$C51/components/Middlewares/radar_ble/include/radar_ble_binding_config.h" \
       "$C52/components/Middlewares/radar_ble/include/radar_ble_binding_config.h"; then
  echo "binding config unexpectedly identical" >&2
  exit 1
fi

echo "C51/C52 radar parity: PASS (binding config is the only expected BLE identity difference)"
