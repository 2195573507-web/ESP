# ESP-111 Gateway Migration Execution Log

## Scope

This log records actual implementation and verification for the staged gateway migration.

Protected paths:

- `ESP-server/`
- `ESP-server/docs/api.md`
- `ESP-server/public/`
- `ESP-server/db/`

## P1: ESPS3 Identity Correction

Status: completed.

Actual work:

- Ran `idf.py set-target esp32s3` for `ESPS3` after cleaning the old C5 build cache.
- Regenerated `ESPS3/sdkconfig` from S3 gateway defaults.
- Replaced `ESPS3` project identity with `sensair_s3_gateway`.
- Replaced the C5 terminal startup chain with `gateway_orchestrator_start()`.
- Added S3 gateway modules: `gateway_config`, `gateway_wifi`, `local_http_server`, `child_registry`, `protocol_adapter`, `server_client`, `sensor_aggregator`, `voice_proxy`, `command_router`, `offline_policy`, and CSI placeholder.
- Replaced the C5 2MB/model partition table with a 32MB gateway table: `nvs`, `otadata`, `phy_init`, two 7MB OTA app partitions, 4MB `storage`, and 256KB `coredump`.
- Removed the direct S3 `esp-sr` dependency from `dependencies.lock`; the S3 gateway build no longer uses wake model config.

Verification:

- Command: `source /Users/zhiqin/.espressif/v5.5.4/esp-idf/export.sh >/dev/null && idf.py set-target esp32s3`
  - Result: success after fixing project component scanning and partition-table CSV syntax.
- Command: `rg -n "CONFIG_IDF_TARGET|CONFIG_ESPTOOLPY_FLASHSIZE|CONFIG_SPIRAM|CONFIG_SR_" sdkconfig sdkconfig.defaults`
  - Result: `CONFIG_IDF_TARGET="esp32s3"`, `CONFIG_ESPTOOLPY_FLASHSIZE="32MB"`, `CONFIG_SPIRAM=y`, `CONFIG_SPIRAM_MODE_OCT=y`, `CONFIG_SPIRAM_SPEED_80M=y`; no active `CONFIG_SR_` defaults remain.
- Command: `source /Users/zhiqin/.espressif/v5.5.4/esp-idf/export.sh >/dev/null && idf.py build`
  - Result: success. Generated `build/sensair_s3_gateway.bin`; app size `0xe7590`, smallest app partition `0x700000`, 87% free.
- Build output flash args include `--chip esp32s3` and `--flash_size 32MB`.

Limits:

- No hardware flash/serial boot log was run in this phase, so PSRAM runtime initialization is verified by sdkconfig/build and boot-time logging code, not by live serial output.
- STA credentials are intentionally empty in defaults; the S3 starts SoftAP/local HTTP and reports server uplink offline until runtime/NVS or compile-time STA credentials are supplied.

## P2-P4: C5 Terminal Local Gateway Migration

Status: completed.

Actual work:

- Added shared C5 runtime configuration module:
  - `ESPC51/components/Middlewares/terminal_config/terminal_config.{h,c}`
  - `ESPC52/components/Middlewares/terminal_config/terminal_config.{h,c}`
- Runtime config reads NVS namespace `terminal_cfg` for `device_id`, `gateway_id`,
  `room_id`, `alias`, `gateway_ssid`, `gateway_pass`, `gateway_ip`, `upload_ms`, and
  `debug_direct`.
- Defaults are intentionally the same in `ESPC51` and `ESPC52`; C51/C52 identity must be
  assigned at runtime/NVS rather than by maintaining two business APIs or two divergent
  firmware implementations.
- Reworked C5 WiFi to STA-only gateway SoftAP mode:
  - `wifi_manager.c` no longer scans a home WiFi list.
  - `wifi_credentials.h` no longer carries home WiFi credentials.
  - C5 connects only to configured S3 SoftAP, default `SensaiHub_S3_01`.
- Reworked C5 communication to local gateway posture:
  - `server_comm_config.c` builds base URL from terminal config gateway IP, default
    `http://192.168.4.1`.
  - `server_comm_http.c` logs as `local_gateway_comm` and sends `X-Gateway-Id`.
  - `server_comm` component compiles `terminal_config` and includes the shared config
    directory.
- Reworked C5 local API envelopes and endpoints:
  - BME690 uploads to `/local/v1/sensor` with `schema_version`, `message_type`,
    `gateway_id`, `device_id`, `room_id`, `alias`, `seq`, `uptime_ms`,
    `timestamp_ms`, `time_synced`, `firmware_version`, `capabilities`, and `payload`.
  - Voice turn endpoint is `/local/v1/voice/turn`.
  - Registration, heartbeat, status, command polling, and ack use `/local/v1/register`,
    `/local/v1/heartbeat`, `/local/v1/status`, `/local/v1/commands/pending`, and
    `/local/v1/commands/{command_id}/ack`.
  - Command parser accepts both `name/payload` and `command_type/params`; unsupported
    commands ack `unsupported_command`.
  - `system_service` sends heartbeat every 5 seconds, status every 15 seconds, and polls
    commands every 5 seconds.
- Disabled default C5 cloud startup side effects:
  - Startup no longer calls cloud time sync by default.
  - Wake prompt download is not started as a default cloud dependency.
- Adjusted C5 voice upload for ESP-IDF httpd compatibility:
  - S3 `httpd_req_t` request bodies require fixed content length for this first phase.
  - C5 now buffers a voice turn up to 320KB and sends fixed `Content-Length` to S3.
  - Added `server_comm_http_post_raw_fixed_stream_begin()` to keep the response playback
    path streaming while avoiding request-side chunked upload.
- Mirrored the C5 implementation to `ESPC52` and verified no source/config difference
  remains between `ESPC51` and `ESPC52` when excluding generated build/component caches.

Verification:

- Command: `cd ESPC51 && source /Users/zhiqin/.espressif/v5.5.4/esp-idf/export.sh >/dev/null && idf.py build`
  - Result: success. Generated `build/00_Learn.bin`; app size `0x13f110`, smallest app
    partition `0x170000`, 13% free.
- Command: `cd ESPC52 && source /Users/zhiqin/.espressif/v5.5.4/esp-idf/export.sh >/dev/null && idf.py build`
  - Result: success. Generated `build/00_Learn.bin`; app size `0x13f110`, smallest app
    partition `0x170000`, 13% free.
- Command: `diff -qr ESPC51 ESPC52 -x build -x managed_components -x .DS_Store`
  - Result: no output; the two C5 source/config trees are identical under this exclusion.
- Command: `rg -n '124\.221\.162\.188|"/api/' ESPC51 ESPC52 --glob '!**/build/**' --glob '!**/managed_components/**'`
  - Result: no output after changing the stale comment in `server_comm_config.h`.
- Command: `rg -n "/local/v1/c51|/local/v1/c52" ESPC51 ESPC52 ESPS3 --glob '!**/build/**' --glob '!**/managed_components/**'`
  - Result: no output.

Limits:

- No physical C5 boot/SoftAP association test was run. Build and static checks prove the
  firmware shape, not live WiFi association.
- The C5 default `device_id` is the same in both directories by design. Production C51/C52
  separation requires NVS provisioning.
- Voice buffering changes heap pressure. The build passes, but real turn length, heap, and
  playback behavior still need serial/runtime validation.

## P5: S3 Sensor/Status Forwarding

Status: completed.

Actual work:

- Implemented S3 local HTTP endpoints in
  `ESPS3/components/Middlewares/local_http_server/local_http_server.c`:
  - `GET /local/v1/health`
  - `POST /local/v1/register`
  - `POST /local/v1/heartbeat`
  - `POST /local/v1/status`
  - `POST /local/v1/sensor`
  - `POST /local/v1/csi/result`
  - `POST /local/v1/voice/turn`
  - `GET /local/v1/commands/pending`
  - `POST /local/v1/commands/*/ack`
- Added child registration/allowlist and envelope parsing/validation through
  `child_registry` and `protocol_adapter`.
- Added `sensor_aggregator` to receive C5 sensor/status envelopes and forward them through
  `server_client`.
- Added `server_client` adapter for existing ESP-server APIs without changing ESP-server:
  - `POST /api/device/v1/ingest`
  - `GET /api/commands/pending`
  - `POST /api/commands/{command_id}/ack`
  - `POST /api/voice/turn`
- `server_client` only performs server requests when S3 STA is connected. If STA is down,
  server calls return degraded state for local error mapping.

Verification:

- Command: `cd ESPS3 && source /Users/zhiqin/.espressif/v5.5.4/esp-idf/export.sh >/dev/null && idf.py build`
  - Result: success. Generated `build/sensair_s3_gateway.bin`; app size `0xe7e60`,
    smallest app partition `0x700000`, 87% free.
- Command: source scan for ESP-server APIs in S3
  - Result: S3-to-server paths are limited to the adapter in `server_client`; C5 source
    has no direct ESP-server URL/API references.

Limits:

- No live ESP-server ingestion smoke was run because this phase must not write the real
  server database.
- S3 default STA credentials are empty, so forwarding is offline until runtime/build-time
  credentials are supplied.

## P6: Voice Proxy And Mutex

Status: completed.

Actual work:

- Implemented gateway voice mutex in `voice_proxy`.
- `POST /local/v1/voice/turn` requires `X-Device-Id`, validates the child against the
  S3 allowlist, accepts fixed-length PCM, forwards to ESP-server `/api/voice/turn`, and
  streams the returned PCM back to the C5 HTTP response.
- Added structured local voice errors:
  - `voice_busy` when another C5 is active.
  - `payload_too_large` when voice body exceeds the configured gateway limit.
  - `gateway_offline` when S3 STA/server path is unavailable locally.
  - `server_unavailable` when the server path fails before audio is returned.
- If audio has already started streaming to C5 and an upstream failure happens, S3 closes
  the chunked response and returns failure rather than mixing JSON into PCM.

Verification:

- Command: `cd ESPS3 && source /Users/zhiqin/.espressif/v5.5.4/esp-idf/export.sh >/dev/null && idf.py build`
  - Result: success.
- Command: `rg -n "voice_busy|server_unavailable|gateway_offline|payload_too_large" ESPS3/components/Middlewares`
  - Result: expected definitions/usages in `voice_proxy` and `offline_policy`.

Limits:

- This is build/static verified only. Real C5 -> S3 -> ESP-server -> S3 -> C5 PCM latency,
  chunk boundaries, and playback quality require hardware testing.
- First-phase C5 voice uploads are buffered before POST, not true streaming upload.

## P7: Local Command Queue, Polling, And Ack

Status: completed.

Actual work:

- Implemented `command_router` local queue with queued/dispatched/acked/timeout states.
- S3 polls existing ESP-server `/api/commands/pending?device_id=...`, maps server command
  names to local terminal command names, and queues them for the matching C5.
- Command mapping:
  - `display.show_text` -> `lcd.show_text`
  - `alert.play_tone` -> `speaker.play_audio`
  - `voice.set_volume` -> `speaker.set_volume`
  - `sensor.set_upload_interval` -> `config.set`
  - Unknown names pass through for forward compatibility.
- C5 polling receives `command_type` and `params`; local ack is converted back to existing
  ESP-server ack shape with `status`, `error_code`, `error_message`, and `result`.
- Oversized command params are rejected with `ESP_ERR_INVALID_SIZE` instead of silently
  truncating JSON.

Verification:

- Command: `cd ESPS3 && source /Users/zhiqin/.espressif/v5.5.4/esp-idf/export.sh >/dev/null && idf.py build`
  - Result: success.
- Command: `cd ESPC51 && source /Users/zhiqin/.espressif/v5.5.4/esp-idf/export.sh >/dev/null && idf.py build`
  - Result: success.
- Command: `cd ESPC52 && source /Users/zhiqin/.espressif/v5.5.4/esp-idf/export.sh >/dev/null && idf.py build`
  - Result: success.

Limits:

- No live command round-trip smoke was run against ESP-server to avoid real DB/server
  mutation in the protected backend.
- Queue checkpoint persistence is not implemented in this phase; the in-memory queue is
  sufficient for first buildable local routing but will not survive S3 reboot.

## P8: Offline Policy, CSI Boundary, Final Static Checks, Rollback Notes

Status: completed.

Actual work:

- Added `offline_policy` to map local/server failures into stable local error codes:
  `gateway_offline`, `timeout`, `voice_busy`, `server_unavailable`, and `server_rejected`.
- `/local/v1/health` reports SoftAP readiness, STA connectivity, server availability,
  voice busy state, and last error code.
- Kept CSI as placeholder/interface only:
  - S3 `csi_placeholder_gateway` logs that raw capture, algorithms, and server upload are
    intentionally unsupported.
  - C5 `sensor_domain/csi_placeholder` remains a reserved service placeholder.
  - Historical note for 2026-06-10: no `esp_wifi_set_csi`, raw CSI collection, algorithm,
    raw upload, or server storage was added in that migration pass. The 2026-06-11
    default-disconnected audit supersedes this as current-state evidence: CSI service
    skeletons may exist, but default startup still does not register WiFi CSI callbacks,
    trigger C5, or ingest/upload CSI results.
- Confirmed the top-level workspace is not a git repository, so `git diff --check` cannot
  be used across firmware directories. Final verification uses `diff -qr`, source `rg`
  checks, and ESP-IDF builds instead.
- Confirmed `ESP-server` is the only git repository under the workspace and remains dirty
  from pre-existing unrelated changes. This migration did not write to `ESP-server`.

Final verification:

- Command: `cd ESPS3 && source /Users/zhiqin/.espressif/v5.5.4/esp-idf/export.sh >/dev/null && idf.py build`
  - Result: success. `sensair_s3_gateway.bin` size `0xe7e60`, app partition free 87%.
  - Flash command shows `--chip esp32s3` and `--flash_size 32MB`.
- Command: `cd ESPC51 && source /Users/zhiqin/.espressif/v5.5.4/esp-idf/export.sh >/dev/null && idf.py build`
  - Result: success. `00_Learn.bin` size `0x13f110`, app partition free 13%.
- Command: `cd ESPC52 && source /Users/zhiqin/.espressif/v5.5.4/esp-idf/export.sh >/dev/null && idf.py build`
  - Result: success. `00_Learn.bin` size `0x13f110`, app partition free 13%.
- Command: `rg -n "CONFIG_IDF_TARGET|CONFIG_ESPTOOLPY_FLASHSIZE|CONFIG_SPIRAM|CONFIG_SR_" ESPS3/sdkconfig ESPS3/sdkconfig.defaults`
  - Result: `CONFIG_IDF_TARGET="esp32s3"`, `CONFIG_ESPTOOLPY_FLASHSIZE="32MB"`,
    `CONFIG_SPIRAM=y`, Octal PSRAM 80MHz, malloc integration and memtest enabled. No
    active `CONFIG_SR_` defaults in `sdkconfig.defaults`.
- Command: `diff -qr ESPC51 ESPC52 -x build -x managed_components -x .DS_Store`
  - Result: no output.
- Command: `rg -n '124\.221\.162\.188|"/api/' ESPC51 ESPC52 --glob '!**/build/**' --glob '!**/managed_components/**'`
  - Result: no output.
- Command: `rg -n "/local/v1/c51|/local/v1/c52" ESPC51 ESPC52 ESPS3 --glob '!**/build/**' --glob '!**/managed_components/**'`
  - Result: no output.
- Command: `rg -n "raw_csi|csi raw|CSI raw|esp_wifi_set_csi|wifi_csi|CSI_DATA|csi_data" ESPC51 ESPC52 ESPS3 --glob '!**/build/**' --glob '!**/managed_components/**'`
  - Result: no output.
- Command: `find . -maxdepth 3 -name .git -type d`
  - Result: only `./ESP-server/.git`.
- Command: `git -C ESP-server status --short --branch`
  - Result: ESP-server is dirty with pre-existing unrelated backend/doc changes; no
    migration command wrote to ESP-server.

Rollback notes:

- S3 rollback is contained in the `ESPS3` directory: restore the previous C5-copy S3 tree
  or keep this S3 gateway tree and disable flashing until hardware validation is done.
- C5 rollback is contained in `ESPC51` and `ESPC52`: restore the previous direct-server
  communication files if a temporary fallback is required. Do not create `/local/v1/c51`
  or `/local/v1/c52` during rollback.
- ESP-server rollback is not applicable because this migration does not modify the server.

Remaining runtime validation:

- Flash S3 and confirm boot logs show PSRAM initialized, detected flash size 32MB, SoftAP
  `192.168.4.1`, and local HTTP service start.
- Provision S3 STA credentials and confirm `server_available` changes from degraded/offline
  to available after successful server requests.
- Provision distinct C51/C52 NVS identities and test both devices joining the S3 SoftAP.
- Run local sensor/status/heartbeat/polling/ack smoke with test devices.
- Run one live voice turn and one concurrent voice contention test to validate `voice_busy`.
