# ESP-111 Gateway Migration Notes

## 2026-06-10 Initial Read

- Top-level `/Users/zhiqin/ESP-111` is not a git repository.
- `ESP-server` is an independent git repository and already has unrelated local changes. It is treated as read-only for this migration.
- `ESPS3` is currently a C5 terminal copy: C5 target, C5 startup chain, C5 terminal middleware, 2MB flash config, no PSRAM.
- ESP-IDF is not on the default PATH. The available export script is `/Users/zhiqin/.espressif/v5.5.4/esp-idf/export.sh`.
- P1 must isolate S3 gateway startup before C5 terminal modules are removed from build.

## 2026-06-10 Implementation Notes

- Top-level `/Users/zhiqin/ESP-111` remains non-git; only `ESP-server` has `.git`.
- `ESP-server` was treated as protected/read-only. It already had unrelated local changes,
  including `docs/api.md`, before this migration completion pass.
- `ESPS3` is now an ESP32-S3 gateway project named `sensair_s3_gateway`; S3 build output
  confirms `--chip esp32s3` and `--flash_size 32MB`.
- S3 N32R16 defaults are in `ESPS3/sdkconfig.defaults` and `ESPS3/partitions.csv`.
  Runtime boot logging in `gateway_config_log_boot_profile()` checks detected flash size,
  PSRAM init state, PSRAM size, and PSRAM heap.
- S3 STA credentials are intentionally empty in defaults. SoftAP/local HTTP can start
  without them, while server forwarding reports offline/degraded until credentials are
  provided.
- C5 communication still uses the `server_comm` component name for compatibility with the
  existing code, but its default role is local gateway communication. It builds URLs from
  NVS/default gateway IP and logs as `local_gateway_comm`.
- C51 and C52 are intentionally identical source/config trees. Device identity must be
  provisioned through NVS keys such as `device_id`, `room_id`, and `alias`.
- C5 voice upload changed from request-side chunked upload to fixed `Content-Length` POST.
  ESP-IDF httpd request handlers in this phase read fixed body length reliably; response
  playback from S3/server remains streamed to the speaker path.
- The C5 voice buffer max is 320KB; the S3 local voice proxy max is 384KB. These limits are
  build-verified but need runtime heap and latency validation on hardware.
- CSI remains placeholder/interface only by default. The source tree may contain CSI service
  skeletons guarded by `MAIN_ENABLE_CSI_SERVICE=0`, but the default startup path does not
  register WiFi CSI callbacks, start CSI tasks, trigger C5 from S3, or upload CSI to Server.
- `managed_components/` and `build/` are generated verification artifacts. Exclude them
  when comparing `ESPC51` and `ESPC52`.
