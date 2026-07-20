# C5/S3 LD2450 P0 Live Source Baseline

Date: 2026-07-17

## Scope

Allowed implementation roots are `ESPC51`, `ESPC52`, and `ESPS3`. This audit
does not modify `ESP-server`, Dashboard, BME690, voice, command, database,
or `managed_components`.

## Initial State

- The top-level worktree was already dirty, including unrelated existing
  changes under protected paths. They are not part of this radar delivery and
  were not reverted, staged, or edited by this work.
- C51 and C52 have independent radar component trees. Their shared radar
  source is checked by `tools/check_c5_radar_parity.sh`; only binding identity
  values may differ.
- C5 uses a NimBLE-oriented `radar_ble` component, a fixed 512-byte byte
  stream, the 30-byte LD2450 parser, and a latest-only local HTTP client.
- ESPS3 already has local UART ingestion through `radar_local_adapter` and
  reusable coordinate, zone, tracker, and spatial-state modules. The remote
  path is added as an adapter into the same domain pipeline.
- ESPS3 local HTTP is the C5-facing boundary. There is no new ESP-server
  route, Dashboard field, database change, or server process started.

## Hardware Facts

| Terminal | local_id | device_id | radar MAC |
|---|---:|---|---|
| C51 | 1 | `sensair_shuttle_01` | `C1:BC:3C:3C:3D:60` |
| C52 | 2 | `sensair_shuttle_02` | `8C:B1:F3:E1:15:41` |

Service UUID, notify UUID, and BLE address type were not supplied. Binding is
therefore deliberately disabled rather than guessed:
`RADAR_BLE_BINDING_ENABLED=0`, `BLOCKED_BY_RADAR_GATT_UUID`.

## Baseline Boundary Decision

The only C5-to-S3 radar interface is `POST /local/v1/radar/result` with the
v2 contract. C5 owns BLE, byte-stream parsing, basic field validation, and
latest-only upload. S3 owns coordinate conversion, zone, tracking, presence,
motion, occupancy, freshness, and source isolation.

