# ESP Backend API Integration Work Plan

## Goal

Complete P0/P1/P2 firmware-side integration from `docs/esp-backend-api-integration-plan.md` while preserving the C5-lightweight/S3-server-facing boundary.

## Constraints

- Primary edits are in `ESPS3`.
- `ESPC51` and `ESPC52` edits are limited to C5->S3 lightweight frame fields and disconnect/reconnect priority.
- C5 must not connect to home WiFi or ESP-server directly.
- Do not upload raw CSI.
- Do not fake real smart-home execution; when no device exists, ACK commands as `failed`.
- Do not add `gateway.heartbeat` to `/api/device/v1/ingest`.
- Do not edit `ESP-server/public`.

## Phases

- [x] Phase 1: Read integration plan and current backend API contract.
- [x] Phase 2: Inspect current S3/C5 firmware modules and existing server-facing endpoints.
- [x] Phase 3: Implement S3 gateway snapshot, status, logs/alarms, smart-home, CSI summary upload, and non-blocking scheduling.
- [x] Phase 4: Add only required C5 lightweight frame/reconnect adjustments and keep C51/C52 aligned.
- [x] Phase 5: Update necessary docs and run syntax/build checks.
- [x] Phase 6: Final scope and completion report.
