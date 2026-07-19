# C5 CSI Feature Schema Notes

## Live Findings

- C51 and C52 CSI source files were byte-identical before this refactor for `csi_types.h`, `csi_feature.h`, `csi_feature.c`, `csi_service.c`, and `csi_server_client.c`.
- Core feature extraction currently produces a flat `csi_feature_result_t` with `frame_energy`, `variance`, `quality`, `rssi`, `frame_seq`, and `timestamp_ms`.
- C5 publish JSON currently exposes flat `frame_energy`, `variance`, `quality`, `rssi`, `frame_seq`, `timestamp`, and a compact `v` array.
- S3 `protocol_adapter_fill_csi_payload()` currently parses flat fields or the compact `v` array, then passes the same low-dimensional fields into `csi_fusion`.
- S3 `csi_placeholder_gateway` already rejects `raw_csi`, `subcarrier_data`, `selected_subcarriers`, `iq`, and `phase` before fusion.

## Chosen Design

- Add an explicit C5 public output schema: `csi_feature_frame_t`.
- Structure each output as `link_id`, `frame_seq`, `timestamp_ms`, `metrics`, and `features`.
- Keep I/Q conversion, selected-subcarrier selection, calibration convergence, filtering/smoothing, and S3 fusion math unchanged.
- Fill `features.motion_score_local` as a local C5 feature score using the legacy local variance/CV weighting formula; it is not used by S3 fusion and does not replace S3-owned `motion_score`.
- Serialize C5 output with nested `metrics` and `features` objects.
- Add S3 schema unpacking for nested `metrics/features`, but do not modify `csi_fusion`.

## Validation

- `diff -u` across C51/C52 CSI type, feature, service, client, and test files returned no differences.
- `git diff --check` passed.
- C5 boundary scan shows active C5 publish JSON emits `metrics` and `features`, not raw CSI, selected subcarriers, or a compact subcarrier/value array.
- `ESPC51`: `. "$IDF_PATH/export.sh" && idf.py build` passed.
- `ESPC52`: `. "$IDF_PATH/export.sh" && idf.py build` passed.
- `ESPS3`: `. "$IDF_PATH/export.sh" && idf.py build` passed.
