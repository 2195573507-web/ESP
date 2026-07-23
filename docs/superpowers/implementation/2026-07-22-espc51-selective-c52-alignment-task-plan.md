# Task Plan: ESPC51 Selective C52 Alignment

## Goal

Apply the approved selective C52 alignment to ESPC51 without changing C51
terminal identity or BLE radar binding.

## Phases

- [x] Phase 1: Approve design and capture scope.
- [x] Phase 2: Identify implementation/configuration boundary.
- [x] Phase 3: Synchronize approved C52 implementation files.
- [x] Phase 4: Verify retained C51 identity and source parity.
- [x] Phase 5: Build ESPC51 in an isolated directory.
- [x] Phase 6: Record results and deliver.

## Constraints

- No flash or monitor.
- Preserve C51 identity, BLE enablement, room, and MAC.
- Preserve existing user changes outside the selected source files.

## Result

The selected C51 implementation files now match C52. Remaining component
differences are C51 identity/radar binding configuration, C51-specific runtime
log labels, and non-source binary or Finder metadata. The isolated ESP-IDF
5.5.4 build completed successfully; the application occupies `0x22b3e0` of a
`0x500000` minimum application partition (57 percent free).
