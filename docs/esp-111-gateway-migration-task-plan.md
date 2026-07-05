# ESP-111 Gateway Migration Task Plan

## Goal

Implement `docs/esp-111-device-roles-and-s3-gateway-migration-plan.md` in strict P1 -> P8 order.

Hard boundaries:

- Do not modify `ESP-server`.
- Do not modify `ESP-server/docs/api.md`, `ESP-server/public`, `ESP-server/db`, or the live database.
- Do not add `/local/v1/c51` or `/local/v1/c52` interfaces.
- CSI remains placeholder/interface only. Do not implement CSI capture, algorithms, raw upload, or server storage.

## Current Status

Current phase: P8 completed; final verification completed.

## Phases

- [x] P1: ESPS3 identity correction, esp32s3 target, N32R16 config, S3 gateway startup, build.
- [x] P2: C5 terminal runtime identity/config model and default no-direct-server posture.
- [x] P3: ESPS3 APSTA, SoftAP `192.168.4.1`, local `/local/v1` HTTP, child registry.
- [x] P4: C5 SoftAP-only WiFi and unified local gateway communication.
- [x] P5: S3 sensor/status forwarding to existing ESP-server APIs.
- [x] P6: C5 -> S3 -> ESP-server -> S3 -> C5 voice path with gateway voice mutex.
- [x] P7: Local command queue, polling, command execution, ack.
- [x] P8: Offline degradation, reconnect policy, error codes, smoke tests, rollback notes.

## Final Phase Summary

P1 changed only `ESPS3` and verified the S3 N32R16 build profile. P2-P4 changed `ESPC51`
and mirrored the same C5 terminal firmware to `ESPC52`; runtime identity is read from
NVS/default terminal config rather than split business interfaces. P5-P8 completed the
S3 local gateway adapter, server forwarding, voice mutex/proxy, command queue, error
mapping, and verification documentation.

No ESP-server source, docs, public assets, db files, or real database were intentionally
modified by this migration. `ESP-server` remains an independent dirty repository with
pre-existing unrelated local changes.

## Verification Gates

Each phase records:

- Files changed.
- Static checks.
- Build command and result.
- Smoke test command and result where feasible.
- ESP-server untouched check.

Final verification commands and results are recorded in
`docs/esp-111-gateway-migration-execution-log.md`.
