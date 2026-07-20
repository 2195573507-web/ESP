# LD2450 migration baseline

Date: 2026-07-16

## Scope

- Allowed roots: `ESPC51/`, `ESPC52/`, `ESPS3/`.
- Forbidden roots: `ESP-server/`, Dashboard/public assets, databases, `managed_components/`, the original `ESP-111`, and radar internal firmware.
- The worktree was already dirty before this migration. Existing audio, voice, backend, runtime, and root-document changes were preserved.
- No flash, monitor, erase-flash, fullclean, Server start, or hardware acceptance was performed.

## Pre-migration active chain

### C51 and C52

```text
WiFi RX callback
-> CSI capture and feature extraction
-> edge state machine
-> runtime worker/event/resource lease
-> local result transport
```

CSI-only source groups present at baseline:

- `sensor_domain/csi_phase_a/`
- `sensor_domain/csi_edge_detector/`
- `sensor_domain/csi_placeholder/`
- `device_protocol/envelope_builder.*`

### S3

```text
periodic trigger
-> C5 result ingress
-> protocol adapter
-> event-bus latest slots and fusion worker
-> fused latest upload
-> Server /kernel/csi_event
```

CSI-only source groups present at baseline:

- `csi_placeholder_gateway/`
- `csi_fusion/`

Shared baseline dependencies included local HTTP, scheduler/event bus, resource manager, sensor aggregator, network worker, server client, and gateway configuration.

## Baseline build

All builds used isolated project-local `build-radar-baseline/` directories because the existing `build/` caches pointed to `/Users/zhiqin/ESP-111`.

| Firmware | Build | Bin bytes | Map image bytes | Primary internal RAM | Flash/data |
|---|---|---:|---:|---:|---:|
| C51 | PASS | 1,376,976 | 1,376,866 | HP SRAM 196,880 | Flash 1,248,682 |
| C52 | PASS | 1,376,976 | 1,376,874 | HP SRAM 196,880 | Flash 1,248,690 |
| S3 | PASS | 1,132,640 | 1,132,529 | DIRAM 175,375 | Code 840,790 + data 172,580 |

## Frozen contracts

- C5 continues to access only S3 local `/local/v1/*` endpoints.
- BME, WiFi, register, heartbeat, status, command, voice, WakeNet, Mic, Speaker, LCD/LVGL contracts remain in place.
- Radar does not create identity, restore sessions, replace heartbeat, upload to Server, or masquerade as another sensor type.
- C51, C52, and S3 local radar states remain independent.

## Hardware baseline

No confirmed UART controller or TX/RX GPIO mapping was available for any of the three boards. Each board therefore has an independent `radar_board_config.h` with UART disabled and port/TX/RX set to `-1`. This is an explicit hardware blocker, not a guessed configuration.
