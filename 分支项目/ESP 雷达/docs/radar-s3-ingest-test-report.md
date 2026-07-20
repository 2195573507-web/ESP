# S3 Radar Ingest Test Report

## Result

`sh ESPS3/components/Middlewares/radar_domain/tests/run_host_tests.sh` passed:

- radar protocol/registry/ingest host tests
- radar spatial/recovery host tests
- S3 radar v2 ingest and source isolation tests

## Verified v2 Ingest Rules

- `POST /local/v1/radar/result` requires a nonempty JSON body no larger than
  1024 bytes and `Content-Type: application/json`.
- Root, value, and target keys are exact and unique. Target count is limited to
  three and must equal array length.
- The C51/C52 `X-Device-Id` must match the body `id` mapping.
- The host test accepts a valid C52 input, rejects identity mismatch, and
  rejects a target with injected `confidence`.
- Duplicate equal requests are idempotent. Sequence conflicts and backward
  sequences are rejected per source.
- An offline C51 update produces C51 `radar_online=false` and occupancy
  `UNKNOWN`; the C52 output remains unchanged.

## Source Isolation

The regression asserts the fixed source IDs `S3_LOCAL=0`, `C51=1`, `C52=2`.
Remote slots are indexed by local id and each owns a separate
`radar_spatial_state_t`; S3 local UART continues through `radar_local_adapter`.

## Downstream Boundary

The handler only reads the bounded local request, validates it, calls
`radar_gateway_ingest`, and returns a limited local response. It does not call
ESP-server or create a new server-facing radar contract. Existing downstream
support for C51/C52 Dashboard display remains unavailable by design:
`BLOCKED_BY_EXISTING_DOWNSTREAM_CONTRACT`.

