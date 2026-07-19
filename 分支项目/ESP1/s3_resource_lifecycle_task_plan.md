# ESPS3 C5 Resource Lifecycle Task Plan

## Goal

Implement device-level C5 session resource lifecycle management in ESPS3 only,
while preserving existing interfaces, the 20-second identity grace period, and
the 30-second heartbeat timeout.

## Scope Boundaries

- Modify only ESPS3 firmware and task-local planning artifacts.
- Do not modify ESP-server, ESPC51, or ESPC52.
- Preserve existing child registry data and protocol compatibility.
- Verify with `idf.py build` from the ESPS3 project.

## Phases

- [x] Create task-local planning files.
- [x] Audit current session, network, scheduler, CSI, command, and sensor paths.
- [x] Implement `resource_manager` and child session state transitions.
- [x] Integrate resource suspend/restore hooks across ESPS3 components.
- [x] Review logging, timeout behavior, compatibility, and touched scope.
- [x] Run `idf.py build` and resolve build failures.

## Errors

- Pre-existing partial implementation allowed release/restore side effects to interleave;
  fixed with a serialized lifecycle operation lock before build verification.
- `idf.py` was not on the default shell PATH; loaded
  `/Users/zhiqin/.espressif/v5.5.4/esp-idf/export.sh` and reran the same build successfully.
