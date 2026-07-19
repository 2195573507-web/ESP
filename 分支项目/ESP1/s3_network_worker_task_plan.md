# S3 Network Worker Refactor Plan

## Goal
Refactor ESPS3 Wi-Fi/APSTA/STA/HTTP/CSI networking so callbacks only update state and enqueue LINK_UP, LINK_DOWN, or IP_READY events. A single network worker owns APSTA/STA gating, local HTTP lifecycle, CSI/UDP enablement, and gateway link state.

## Phases
- [x] Phase 1: Map current ESPS3 Wi-Fi, HTTP, CSI, UDP paths.
- [x] Phase 2: Add network worker event API and APSTA stability gate.
- [x] Phase 3: Move Wi-Fi callback business logic into the worker.
- [x] Phase 4: Gate HTTP, CSI trigger/fusion, UDP listener/sender through the worker.
- [x] Phase 5: Validate callback boundaries and build ESPS3.

## Notes
- Existing workspace has unrelated dirty changes; keep edits scoped to ESPS3 network modules.
- APSTA dual stack remains enabled.
- CSI/upload paths require at least 3 seconds of stable network state before being enabled.
- Verification passed: `git diff --check` on touched ESPS3 network files, callback grep, and ESPS3 `idf.py build` using ESP-IDF v5.5.4 with `IDF_PYTHON_ENV_PATH=/Users/zhiqin/.espressif/tools/python_env/idf5.5_py3.14_env`.
