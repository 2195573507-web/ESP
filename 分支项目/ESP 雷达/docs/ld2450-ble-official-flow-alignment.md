# LD2450 BLE Official Flow Alignment

- Date: 2026-07-18
- Scope: `ESPC51` and `ESPC52` LD2450 BLE transport state machine only.
- Evidence: `/Users/zhiqin/Documents/ESP 资料/docs/ld2450-ios-control-command-analysis.md`.
- Excluded: parser, filter, upload, and ESPS3 are unchanged.

## Official-App Evidence

The official iOS source identifies the LD2450 FFF0 profile as FFF1 Notify and
FFF2 Write. Its LD2450 path discovers those characteristics, enables FFF1
notifications, and then displays incoming raw notifications. It contains no
FFF2 write that starts the radar stream, and it provides no LD2450 FFF1
response format that would acknowledge such a start command.

Therefore this alignment does not infer or send an FFF2 payload. Obtaining a
future control payload requires a device-specific ATT/GATT capture that shows
the write handle, write type, bytes, and response semantics.

## Applied C5 Flow

```text
discover
  -> profile_found
  -> subscribe_FFF1
  -> ready
```

1. Full GATT discovery still selects FFF1 only when it advertises Notify and
   records FFF2 dynamically only when it has the supported write property.
2. After the FFF1 profile is found, the client discovers FFF1's CCCD and writes
   standard notification enable value `01 00`.
3. A successful FFF1 CCCD write sets `notify_subscribed=true`,
   `data_ready=true`, and the BLE state to `ready`.
4. READY and runtime link-online now require only `connected` and
   `notify_subscribed`; they do not wait for an FFF2 write or a synthetic FFF1
   control response.

The subscription completion emits:

```text
RADAR_BLE_NOTIFY_SUBSCRIBED service=FFF0 notify=FFF1
RADAR_BLE_WAIT_DATA
```

## FFF2 Boundary

FFF2 handle discovery remains in `select_profile_characteristics()`. The
explicit `radar_ble_set_control_command()` and
`radar_ble_send_control_command()` APIs remain available for a future,
evidence-backed control feature. They have no caller in the connection path,
and this change supplies no payload.

## Files Changed

- `ESPC51/components/Middlewares/radar_ble/radar_ble_transport.c`
- `ESPC51/components/Middlewares/radar_ble/radar_ble_runtime.c`
- `ESPC51/components/Middlewares/radar_ble/include/radar_ble_transport.h`
- Identical shared changes under `ESPC52/components/Middlewares/radar_ble/`.

## Verification Scope

Static checks must prove C51/C52 shared BLE parity, no discovery-path call to
the FFF2 control sender, and the required subscription logs. Build checks
validate the two firmware projects separately. Neither check proves that an
actual LD2450 begins notifying after subscription; that remains device
validation work.
