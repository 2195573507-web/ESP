# C5 LD2450 BLE Control Flow Audit

- Date: 2026-07-18
- Scope: read-only audit of `ESPC51` and `ESPC52` under
  `components/Middlewares/radar_ble` and `components/radar_ld2450`.
- Excluded: no firmware code, parser, ESPS3, or transport/protocol contract was
  changed. No build, flash, monitor, or device connection was performed.
- Evidence labels: **A - source confirmed**; **B - official-App flow supplied
  for this audit**; **C - requires GATT/ATT capture on the actual device**.

## Conclusion

**A** Both C5 projects currently implement a notify-only LD2450 flow: target
MAC scan, connect, whole-GATT survey, find notify characteristic `ae02`, write
its CCCD with `01 00`, then forward received notifications to the LD2450 byte
stream and parser. They do not configure, discover, or write `ae01`.

**B/C** Against the supplied official-App sequence `connect -> ae01 control ->
ae02 notify`, the current C5 implementation is missing the radar-start control
step. The source does not contain the `ae01` UUID, its GATT properties, the
start-command bytes, or an acknowledgement rule. Those details must be
captured from the exact LD2450 firmware before an implementation is authorized.

## C51/C52 Consistency

**A** `radar_ble_transport.c`, `radar_ble_runtime.c`, and
`radar_ble_stream.c` are byte-identical between C51 and C52. The binding
headers differ only in identity and MAC values; both set
`RADAR_BLE_BINDING_ENABLED` to `1`, `RADAR_BLE_BINDING_NOTIFY_UUID` to `ae02`,
and `RADAR_BLE_BINDING_WRITE_UUID` to an empty string. The relevant parser and
service code have the same control-path behavior; C52 has an additional parsed
frame diagnostic log, which does not affect BLE control.

## Current BLE Flow

**A** The shared current flow is:

```text
radar_ble_runtime_start()
  -> radar_domain_start() / radar_service_start()
  -> radar_ble_transport_start(notify_cb)
  -> NimBLE sync -> scan exact configured MAC
  -> connect
  -> service/characteristic discovery
       (service UUID is empty, so survey all services)
  -> select characteristic UUID ae02 only when it has Notify property
  -> discover ae02 descriptor 0x2902 (CCCD)
  -> write CCCD value 01 00
  -> set notify_subscribed=true
  -> BLE_GAP_EVENT_NOTIFY_RX from ae02
  -> radar_domain_notify() -> bounded raw buffer
  -> radar_service_ingest_ble_bytes() -> 30-byte LD2450 parser
```

Source anchors:

| Step | Evidence |
| --- | --- |
| Runtime starts the BLE transport and supplies the notification callback | `ESPC51/components/Middlewares/radar_ble/radar_ble_runtime.c:23-27,55-77`; identical in C52 |
| Exact-MAC scan, connect, and all-service survey when service UUID is empty | `ESPC51/components/Middlewares/radar_ble/radar_ble_transport.c:341-388,471-476` |
| `ae02` is selected only when its characteristic has Notify property | `ESPC51/components/Middlewares/radar_ble/radar_ble_transport.c:295-306` |
| CCCD is located and written with `01 00` | `ESPC51/components/Middlewares/radar_ble/radar_ble_transport.c:179-203` |
| Notifications are accepted only from the selected `ae02` value handle and forwarded | `ESPC51/components/Middlewares/radar_ble/radar_ble_transport.c:390-418`; `radar_ble_runtime.c:23-27` |
| Buffered notify bytes reach the LD2450 parser | `ESPC51/components/Middlewares/radar_domain/radar_worker.c:196-200,317-324`; `ESPC51/components/radar_ld2450/radar_service.c:135-139` |

The C5 transition to `READY` is a local state change after CCCD subscription;
it is not a radar-start transaction. `radar_ble_transport_set_data_ready()`
only combines its boolean input with `notify_subscribed`.

## ae01 Status

| Question | Finding |
| --- | --- |
| Is `ae01` configured? | **A: No.** No C51/C52 source under the audited components contains `ae01`. `RADAR_BLE_BINDING_WRITE_UUID` is `""` in both binding headers. |
| Is `ae01` discovered? | **A: No.** Characteristic discovery records `s_write_handle` only when the write UUID configuration is non-empty. Because it is empty, `s_write_handle` remains zero. See `radar_ble_transport.c:226-234`. |
| Is `ae01` written? | **A: No.** The only executed GATT write is the CCCD write. `radar_ble_transport_write()` is a generic dormant API; repository search finds only its declaration and definition, no caller. It rejects the call when `s_write_handle == 0`. See `radar_ble_transport.c:192-195,499-509`. |
| What data is written to `ae01`? | **A: None.** There is no `ae01` payload constant, frame builder, command enum, or call site. **C:** the actual start-command byte sequence is not derivable from this source. |
| When is `ae01` written? | **A: Never.** The present post-discovery operation is CCCD subscription only. |
| What is its use? | **B:** In the supplied official-App sequence, `ae01` is the control channel used before `ae02` streaming. **C:** source inspection alone cannot prove whether it is Write, Write Without Response, whether it carries an enable/start command, or its required acknowledgement and retry behavior. |

The generic write abstraction must not be mistaken for an implemented control
path. Its existence only shows that a future, verified write characteristic can
be supported.

## ae02 Status

| Item | Finding |
| --- | --- |
| UUID | **A:** Both projects configure `ae02` as `RADAR_BLE_BINDING_NOTIFY_UUID`. |
| Discovery | **A:** The unconfigured-service mode surveys every service, logs the discovered characteristic UUID/properties, and keeps `ae02` only if its Notify bit is set. |
| Subscription write | **A:** The client discovers the Client Characteristic Configuration Descriptor (`0x2902`) for `ae02` and writes little-endian `01 00`, which enables standard GATT notifications. This is a CCCD descriptor write, not a write to `ae02` itself. |
| Receive path | **A:** After the CCCD response succeeds, `notify_subscribed` becomes true. Only Notify events whose attribute handle equals the stored `ae02` handle are copied and forwarded. |
| Payload semantics | **A:** C5 treats the payload as a byte stream and parses complete 30-byte LD2450 frames downstream. **C:** this audit did not capture a real `ae02` notification, so it does not prove current-device payload timing or first-frame behavior. |

## Comparison With the Official-App Flow

| Sequence | Official-App flow supplied for this audit | Current C51/C52 source | Result |
| --- | --- | --- | --- |
| 1 | Connect | Exact-MAC scan then `ble_gap_connect()` | Present |
| 2 | Discover control and notification characteristics | Surveys services only to retain `ae02` Notify; does not retain `ae01` | Incomplete |
| 3 | Write `ae01` radar control/start command | No `ae01` handle, command bytes, write call, or command acknowledgement | **Missing** |
| 4 | Subscribe/receive `ae02` notify | Discovers CCCD and writes `01 00`; receives from `ae02` | Present |
| 5 | Process streamed radar bytes | Buffers and feeds bytes to the fixed-30-byte LD2450 parser | Present after notify arrives |

Therefore the answer to the audit question is **yes**: by source evidence, the
current implementation has only the `ae02` CCCD-subscribe action and lacks the
`ae01` radar-start command. A successful `RADAR_NOTIFY_ENABLED` log proves
only the CCCD transaction, not that the radar was explicitly started.

## Missing Steps and Safe Insertion Point

No code is changed by this audit. The following is a future implementation
placement recommendation, conditional on captured device evidence.

1. **Capture first (C):** use the official App with the same LD2450 firmware
   and record complete GATT discovery plus ATT writes. Confirm service UUID,
   `ae01` and `ae02` properties/value handles, write type, exact command bytes,
   whether a response/acknowledgement is expected, and whether control must
   precede CCCD subscription exactly as observed.
2. **Binding configuration:** add the verified service UUID, `ae01` write UUID,
   write mode, and named immutable start-command bytes beside the existing
   `RADAR_BLE_BINDING_*` constants in each C5 binding header. C51/C52 should
   remain byte-identical apart from their established identity/MAC bindings.
3. **Characteristic discovery:** in `radar_ble_transport.c`, extend both the
   configured-service callback (`chr_cb`, lines 206-235) and all-service survey
   callback (`survey_chr_cb`, lines 282-309) to record the verified `ae01`
   value handle and validate its required write property. Do not select a
   same-named or arbitrary writable characteristic.
4. **Transaction ordering:** after discovery has produced both the `ae01`
   handle and the `ae02` CCCD handle, create one control-transaction entry
   point. For the stated App sequence, it should write the verified start
   bytes to `ae01`; only its verified success/ack path should issue the
   existing CCCD `01 00` write. The natural current seam is the
   `BLE_HS_EDONE` branch of `dsc_cb` (lines 187-197), replacing its direct CCCD
   write with `start-control -> subscribe` callbacks.
5. **Failure behavior:** an `ae01` write failure, rejected acknowledgement, or
   missing handle should take the existing bounded `schedule_retry()` path and
   must not set `notify_subscribed` or `READY`. Log handle, operation phase,
   GATT status, and a bounded payload fingerprint, not an invented success.

Do not add a guessed `ae01` UUID, guessed command bytes, or a fallback that
subscribes despite an unconfirmed control failure. Those would defeat the
current MAC-bound, inspectable control boundary and could change behavior on a
different LD2450 firmware revision.

## Verification Needed Before Any Change

- **C:** One packet-level App capture showing connection, characteristic
  discovery, `ae01` write request/response, and `ae02` CCCD write/first notify.
- **C:** Confirm the exact module firmware version and address type for each
  bound radar; local documentation warns that LD2450 BLE UUID arrangements vary
  by firmware.
- **C:** On both C51 and C52, verify the expected ordering, startup latency,
  reconnect behavior, and no missed first frames after the control command.
- **A:** After a future source change, statically prove that the only
  `radar_ble_transport_write()` caller is the explicit verified control
  transaction, then build C51 and C52 separately before hardware validation.

