# LD2450 BLE UUID Compatibility Plan

## Scope

- Applies only to the shared C51/C52 `radar_ble` transport and its binding
  configuration.
- Does not modify the LD2450 parser, filter, ESPS3, or any HTTP contract.
- No flash is part of this change. Source and build checks are software-only.

## Profile Discovery

The central always discovers all GATT services and traverses every discovered
characteristic. It never filters discovery by a configured service UUID.

The selected notification profile is the highest-priority characteristic that
has the required property:

| Priority | UUID | Required property |
| --- | --- | --- |
| 1 | `FFF1` | Notify |
| 2 | `ae02` | Notify |

The selected control profile is likewise property-qualified:

| Priority | UUID | Required property |
| --- | --- | --- |
| 1 | `FFF2` | Write Without Response |
| 2 | `ae01` | Write |

This covers the standard LD2450 `FFF0` service (`FFF2` write, `FFF1` notify)
while retaining discovery of the existing `ae00` service family. `ae00` is not
hard-coded as a discovery filter; it is reached through all-service traversal.

The transport stores the selected notification and control value handles. It
emits `RADAR_BLE_PROFILE_FOUND` with the selected service UUID, write UUID and
handle, and notify UUID and handle. A standard profile therefore logs the
equivalent of `service=FFF0 write_uuid=FFF2 notify_uuid=FFF1`.

## Startup Control State

The connection sequence is:

```text
discover -> control_start -> subscribe -> ready
```

After characteristic discovery, `radar_ble_start_control()` is the sole entry
point for control sequencing.

- With a discovered write handle, it enters `control_start` and uses the
  discovered property to choose Write Without Response (`FFF2`) or Write
  (`ae01`). Only a successful control transaction may continue to CCCD
  discovery and the `01 00` notification subscription.
- The start payload is intentionally not invented. Both bindings define
  `RADAR_BLE_START_COMMAND_PLACEHOLDER=1`, an empty command length, and no
  payload is sent. In this state a discovered control channel logs
  `RADAR_BLE_START_COMMAND_PLACEHOLDER` and takes the existing retry path.
- With no discovered write handle, the transport logs
  `RADAR_BLE_NO_CONTROL_CHANNEL` and falls back to notification-only CCCD
  subscription. This preserves compatibility with notify-only `ae02` devices.

Replacing the placeholder requires an actual iOS/App GATT capture that proves
the complete command bytes and any device-specific acknowledgement rule. It is
not authorized by this compatibility change.

## C51/C52 Parity

The transport, runtime, stream, parser-facing service, and shared headers
remain byte-identical. The binding and board configuration headers differ only
in MAC, `device_id`, and `local_id`; room, radar, and board labels are shared to
keep this boundary explicit.

## Software Verification

1. Run `tools/check_c5_radar_parity.sh`.
2. Run `tools/test_c5_radar_ble_stream.sh`.
3. Build C51 and C52 in their isolated `build-radar-c51` and `build-radar-c52`
   directories.
4. Run `git diff --check` over the changed C51/C52 BLE files and this document.

These checks establish source and host-build compatibility only. Actual iOS
interoperability, write-command semantics, first-notify timing, and reconnect
behavior still require hardware validation without flashing as part of this
task.
