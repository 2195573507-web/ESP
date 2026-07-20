#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUTPUT="${TMPDIR:-/tmp}/esps3_radar_log_parser_checks"
FIXTURE="$ROOT_DIR/Samples/three-room-mixed.log"
S3_LOCAL_FIXTURE="$ROOT_DIR/Samples/s3-local-current.log"

swiftc "$ROOT_DIR/Sources/Models/RadarModels.swift" \
       "$ROOT_DIR/Sources/Models/RadarSource.swift" \
       "$ROOT_DIR/Sources/Models/RadarDashboardLayout.swift" \
       "$ROOT_DIR/Sources/Stores/RadarStateStore.swift" \
       "$ROOT_DIR/Sources/Services/RadarReplayController.swift" \
       "$ROOT_DIR/Sources/Services/RadarLogParser.swift" \
       "$ROOT_DIR/Sources/Services/ESPS3RadarLogParser.swift" \
       "$ROOT_DIR/script/ParserChecks/main.swift" \
       -parse-as-library \
       -o "$OUTPUT"
"$OUTPUT" "$FIXTURE" "$S3_LOCAL_FIXTURE"
