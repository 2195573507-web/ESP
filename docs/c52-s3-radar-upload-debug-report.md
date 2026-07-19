# C52 -> S3 Radar Upload Debug Report

## Scope

This change adds diagnostic logs only in `ESPC52` and `ESPS3`. It does not alter BLE connection behavior, LD2450 parsing, coordinate transformation, zone mapping, tracking, spatial state, or any ESP-server, Dashboard, voice, or BME690 code.

## 1. C52 BLE Status

The requested runtime prerequisites are accepted as the current hardware observation: BLE scan, MAC match, LD2450 GATT connection, and `ae02` notification subscription are successful. This change does not modify those paths.

## 2. C52 Parser Status

After every successfully parsed LD2450 frame, C52 now emits `RADAR_FRAME_PARSED` with `local_id`, parsed target count, and the first target's `target_id`, `x_mm`, `y_mm`, `velocity_cm_s`, `distance_mm`, and `confidence`. It deliberately does not log the raw 30-byte frame.

## 3. C52 Upload Status

C52's compiled `radar_domain/radar_worker.c` upload path now emits:

- `RADAR_RESULT_ENCODE` immediately before v3 result encoding, with local id, target count, and request sequence.
- `RADAR_RESULT_UPLOAD` immediately before the POST to `/local/v1/radar/result`, with payload byte count.
- `RADAR_RESULT_UPLOAD_DONE` after every POST attempt, with the ESP-IDF transport status and HTTP response code.

Together these logs distinguish parser success, encode failure, upload attempt, transport failure, and HTTP response failure. `sensor_domain/radar/radar_state_client.c` also contains an older upload implementation, but it is absent from the CMake source list and is not part of the current firmware image; it was not used for these diagnostics.

## 4. S3 Receive Status

For a successfully parsed HTTP body, S3 now emits `RADAR_HTTP_INGEST` with source (`C51` or `C52`), local id, target count, and received payload size. After it queues the latest sample, it emits `RADAR_INGEST_ACCEPT` with source, target count, and request sequence. Parser, availability, and downstream gateway-admission failures emit `RADAR_INGEST_DROP reason=<reason>`.

The active handler is `local_http_server/radar_local_handler.c`, while the CMake build uses `Middlewares/radar_ingest/radar_ingest.c`. The duplicate `radar_domain/radar_ingest.c` is not included in the `Middlewares` component source list and was not changed.

## 5. Protocol Consistency

Static source comparison result: compatible.

| Area | C52 encoder | S3 parser | Result |
| --- | --- | --- | --- |
| Envelope | `p`, `id`, `t`, `u`, `q`, `v` | Requires exactly the same keys | Match |
| Required values | `p=3`, `t="radar"`, `id=1..2`, nonzero `q` | Requires the same ranges and literal values | Match |
| Target fields | `target_id`, `x_mm`, `y_mm`, `velocity_cm_s`, `distance_mm`, `confidence` | Requires the same fields | Match |
| Additional target field | `resolution_mm` | Requires `resolution_mm` exactly | Match; this extra required field is outside the requested six-field list |

There are no C52/S3 differences in the checked envelope or target fields.

## 6. Next Repair Location

Run the C52 and S3 logs together during one notification cycle. The first missing marker identifies the repair boundary:

1. No `RADAR_FRAME_PARSED`: C52 parser or notification-to-parser handoff.
2. Parsed but no `RADAR_RESULT_ENCODE`: C52 reporting-task scheduling or gateway readiness gate.
3. Encoded but no `RADAR_RESULT_UPLOAD`: encode failure in the compiled `radar_worker` path.
4. Upload but no `RADAR_RESULT_UPLOAD_DONE`: HTTP call/transport timeout path.
5. Upload done but no `RADAR_HTTP_INGEST`: C52-to-S3 network route, address, or S3 HTTP-server availability.
6. HTTP ingest followed by `RADAR_INGEST_DROP`: inspect the explicit S3 validation or gateway-admission reason.

Static repair candidate outside this diagnostic-only change: if the intended C52 path was `sensor_domain/radar/radar_state_client.c`, it must first be added to `Middlewares/CMakeLists.txt` and started by runtime code. That would enable a second uploader and change behavior, so it is intentionally not done here.
