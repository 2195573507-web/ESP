# ESPS3 Radar Debug for macOS

## Goal

Create a standalone native macOS application, `ESPS3RadarDebug.app`, for viewing LD2450 targets from the ESPS3 USB serial console. It must be usable by double-clicking the app bundle: the user selects a local serial device and presses `启动本地解析`; no terminal commands, ESPS3 HTTP route, or firmware modification is required.

The visual layout follows the existing `HLK2450Mac` real-time radar view but is an independent project and does not send LD2450 commands.

## Scope

- Input: text logs from an ESPS3 serial console connected to the Mac, defaulting to 115200 baud.
- Output: a 6 m semicircular radar plot, radar origin, coloured current target markers, recent per-track trails, target detail cards, serial connection state, and parse counters.
- Control: one primary `启动本地解析` / `停止本地解析` button; opening the port starts parsing and closing it stops parsing.
- Packaging: a SwiftPM executable wrapped in `dist/ESPS3RadarDebug.app` with a Finder-launchable build artifact.

Out of scope: direct LD2450 UART protocol parsing, sending any serial bytes, radar configuration, zones, room mapping, dashboard/server access, firmware changes, and C51/C52 support.

## Source Log Contract

The parser consumes the ESP-IDF text stream directly. It finds `local sensor=` and complete `local track=... missed=...` records without depending on a CR, LF, or CRLF line ending, so serial driver chunking cannot split a record incorrectly.

`local sensor=...` starts a new diagnostic snapshot. Each following visible track line is parsed from this stable shape:

```text
local track=<id> visible=<0|1> raw_x=<mm> raw_y=<mm> filtered_x=<mm> filtered_y=<mm> distance=<mm> angle=<deg> speed=<cm/s> direction=<deg> confidence=<n> seen=<n> missed=<n>
```

The app uses `filtered_x`, `filtered_y`, `distance`, `angle`, `speed`, `confidence`, `visible`, and `id` directly from that line. It assigns the local receive time as the sample timestamp. Active but `visible=0` tracks remain visible to the debugger with a muted marker. A malformed or unrelated log line is ignored and counted; it cannot change the current target view.

## Architecture

```text
ESPS3-Radar-Debug/
  Package.swift
  Sources/
    App/ESPS3RadarDebugApp.swift
    Models/RadarModels.swift
    Services/SerialPort.swift
    Services/ESPS3RadarLogParser.swift
    Stores/RadarStore.swift
    Views/ContentView.swift
    Views/DashboardView.swift
    Views/RadarCanvas.swift
  script/ParserChecks/main.swift
  script/run_parser_checks.sh
  script/build_app.sh
  dist/ESPS3RadarDebug.app
```

- `SerialPort` lists both `/dev/tty.*` and `/dev/cu.*`, prioritizes USB `tty` devices because ESPS3 console logs are commonly attached there, opens one selected console in read-only behavior, and emits byte chunks.
- `ESPS3RadarLogParser` owns incremental UTF-8 line framing and produces validated snapshots. It has no UI or serial dependency.
- `RadarStore` owns connection state, current targets, 120 samples of per-target history, received-byte/completed-line counters, a short raw-byte preview, and the start/stop action.
- `RadarCanvas` renders the native SwiftUI plot with unmodified parsed coordinates. Target colour remains stable for each ESPS3 `track_id`.
- `DashboardView` contains the serial picker, baud picker, primary parsing button, canvas, and target/diagnostics panel.

## Interaction and Failure Handling

- On launch, the app lists serial devices and selects the highest-priority USB `tty` device. The default baud is 115200, but the user may choose another console rate before starting.
- `启动本地解析` is disabled until a serial device is selected. Its action opens the port and begins line parsing; the label immediately changes to `停止本地解析`.
- Disconnect, open failure, or invalid UTF-8 transitions the status field to an error without crashing or retaining a live connection.
- When a new `local sensor=` line arrives, current targets clear before tracks from that next snapshot are received. A snapshot with no visible tracks therefore displays no targets instead of stale markers.
- `清除轨迹` clears local app history only. It does not send a command to ESPS3.

## Verification

1. Run the standalone parser check for complete, fragmented, prefixed, malformed, and non-visible ESPS3 track-log lines. This avoids depending on a system XCTest module.
2. Run `script/run_parser_checks.sh` and `swift build`.
3. Run `script/build_app.sh` to create the Finder-launchable app bundle.
4. Open the generated `.app` and verify it remains running. Live target rendering requires an actual ESPS3 console and is not claimed by build-only validation.

## Acceptance Criteria

- A standalone `ESPS3RadarDebug.app` is produced without modifying `HLK2450Mac` or ESPS3.
- The user can double-click the app, choose the ESPS3 USB console, and use one visible start button to begin parsing.
- Valid `local track=` logs create current targets and trails with the logged ID, X/Y, distance, angle, speed, confidence, and local timestamp.
- Invalid or unrelated logs do not corrupt the visual state.
