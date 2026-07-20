# Radar Resource and Safety Report

## C5 Static Design

| Item | C51/C52 value | Assessment |
|---|---:|---|
| BLE stream | 512 bytes | fixed, latest bytes only |
| BLE/parser task | 4096-byte stack, priority 2 | parses outside callback |
| radar upload task | 4096-byte stack, priority 3 | owns blocking local HTTP and latest-only retry |
| pending observation | one fixed latest slot | older observations coalesce |
| normal report cadence | 100 ms minimum | maximum 10 Hz |
| status heartbeat | 1000 ms | disconnected status is best effort |

The two C5 radar tasks are intentional: the parser must continue draining the
fixed stream while a bounded synchronous HTTP request is waiting. This is the
minimal separation that preserves the plan's nonblocking BLE-input rule; no
BLE callback performs JSON or HTTP.

No explicit `malloc`, `calloc`, `realloc`, or `free` call exists in the C5
radar parser, BLE stream, or codec paths. The parser, edge sample, mutex, and
stream are fixed storage. Heap delta and actual stack high-water require a
flashed device and are not claimed here.

## S3 Static Design

| Item | Value | Assessment |
|---|---:|---|
| request body bound | 1024 bytes | rejected before ingest over this limit |
| remote sources | 2 x 3 targets | fixed C51/C52 slots |
| S3 sources | 3 | independent `S3_LOCAL`, C51, C52 contexts |
| remote gateway poll task | 4096-byte stack, priority 2 | freshness only; no network upload task |
| local adapter/diagnostics | 4096-byte stacks | existing S3 local radar infrastructure |

Tracker, association, coordinate, zone, and spatial-state algorithms use
fixed arrays (three targets/tracks) and do not allocate per frame. The local
HTTP handler intentionally allocates a bounded request body and cJSON parse
tree per HTTP request, then frees both before response; this transport parsing
allocation is not part of the per-frame radar algorithm and is bounded by the
1024-byte body limit.

## Static Scan Classification

- No C5 `/api`, `/local/v1/sensor`, or `/local/v1/csi` radar transport hit.
- No raw frame/base64 upload path exists.
- `confidence` hits are either negative tests, the v2 rejection test, or
  S3-only tracker/spatial lifecycle evidence. C5 payload generation has none.
- No C5 radar CRC/checksum assumption exists; validation uses fixed length,
  header, tail, and re-synchronization.
- Latest build logs contain no warning. Historical log files retain old warning
  text and are not evidence of the final builds.

