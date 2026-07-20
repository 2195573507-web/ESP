# C5 Parser and BLE Mock Test Report

## Result

| Check | Result |
|---|---|
| C51 parser and v2 codec host test | PASS |
| C52 parser and v2 codec host test | PASS |
| C5 BLE stream host test | PASS |
| C51/C52 radar parity | PASS |

Commands run:

```sh
sh ESPC51/components/radar_ld2450/tests/run_host_tests.sh
sh ESPC52/components/radar_ld2450/tests/run_host_tests.sh
sh tools/test_c5_radar_ble_stream.sh
sh tools/check_c5_radar_parity.sh
```

## Covered Cases

- Official signed target vector: `X=-782`, `Y=1713`, `speed=-16`,
  `resolution=320`, `distance=1883` mm.
- Full frame, byte-at-a-time stream, joined frames, partial frames, garbage
  prefix re-synchronization, invalid tail, timeout reset, all-zero slots, and
  three target slots.
- Integer distance overflow boundary and v2 JSON codec field/size validation.
- Codec does not generate `confidence` or `raw` fields; it rejects a too-small
  body buffer.
- The BLE stream keeps only the latest 512 bytes. The 600-byte mock overflow
  increments overflow/resync counters and does not allocate a per-notify heap
  object.
- Parity permits only C51/C52 binding identity differences: local id,
  device id, and radar MAC.

## BLE Limitation

The state-machine and stream behavior are host-tested. No real scan,
connection, GATT discovery, CCCD subscription, address type, or notification
was exercised because service UUID, notify UUID, and address type are unknown.
Status: `BLOCKED_BY_RADAR_GATT_UUID`.

