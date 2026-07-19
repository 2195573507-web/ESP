# Task Plan: ESPS3 Serial Status Debug

## Goal
Read the ESP32-S3 gateway status over serial, identify the current failure, and apply the smallest safe fix.

## Phases
- [x] Phase 1: Confirm repo layout and visible serial ports
- [x] Phase 2: Create persistent debug notes
- [x] Phase 3: Capture ESPS3 serial logs
- [x] Phase 4: Diagnose server-path state issue from logs and source
- [x] Phase 5: Apply minimal server-state/logging fix
- [ ] Phase 6: Isolate and fix 30-second S3 panic
- [ ] Phase 7: Verify with build, flash, and serial smoke check

## Current Notes
- Workspace: `/Users/zhiqin/ESP-111`
- Top-level repo plus nested `ESP-server` repo are both present.
- ESPS3 target: `esp32s3`
- ESPS3 console baud: `115200`
- Visible serial candidates:
  - `/dev/cu.usbmodem141101`
  - `/dev/cu.usbmodem14301`
  - `/dev/cu.usbmodem5B901601171`
- `/dev/cu.usbmodem141101` is the confirmed S3 port, MAC `90:e5:b1:cc:ee:40`.
- After flashing the diagnostic build, S3 reaches `server=1` and uploads `schema=2 payload_type=gateway.dashboard_snapshot`.
- Remaining blocker: S3 still panics with `StoreProhibited` around the 30-second heartbeat window.
