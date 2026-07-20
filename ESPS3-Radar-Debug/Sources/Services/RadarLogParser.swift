import Foundation

struct RadarLogParser {
    private var pendingText = ""
    private var mostRecentSource: RadarSource?

    private static let trackPattern = try! NSRegularExpression(
        pattern: #"(?:local\s+)?track=\d+\s+visible=[01]\s+raw_x=-?\d+\s+raw_y=-?\d+(?:\s+filtered_x=-?\d+\s+filtered_y=-?\d+)?\s+distance=\d+\s+angle=-?\d+\s+speed=-?\d+(?:\s+direction=-?\d+)?\s+confidence=\d+(?:\s+seen=\d+\s+missed=\d+)?"#,
        options: [.caseInsensitive]
    )

    private static let compactTrackPattern = try! NSRegularExpression(
        pattern: #"(?:local\s+)?track=(?:(\d+)\s+)?x:(-?\d+)\s+y:(-?\d+)"#,
        options: [.caseInsensitive]
    )

    private static let legacyFrameMarker = try! NSRegularExpression(
        pattern: #"(?:^|\s)local\s+sensor="#,
        options: [.caseInsensitive]
    )

    mutating func consume(_ bytes: [UInt8]) -> [RadarLogEvent] {
        pendingText += String(decoding: bytes, as: UTF8.self)
        if pendingText.utf8.count > 32_768 {
            pendingText = String(pendingText.suffix(8_192))
        }

        var events: [RadarLogEvent] = []
        while let newline = pendingText.firstIndex(where: { $0 == "\n" || $0 == "\r" }) {
            let line = String(pendingText[..<newline])
            pendingText.removeSubrange(...newline)
            events.append(contentsOf: parseRecord(line))
        }

        // Preserve the former parser's ability to recognize complete records without a line ending.
        while true {
            let sensorRange = Self.legacyFrameMarker.firstMatch(
                in: pendingText,
                range: NSRange(pendingText.startIndex..., in: pendingText)
            ).flatMap { Range($0.range, in: pendingText) }
            let match = Self.trackPattern.firstMatch(in: pendingText,
                                                     range: NSRange(pendingText.startIndex..., in: pendingText))
            let trackStart = match.flatMap { Range($0.range, in: pendingText)?.lowerBound }
            if let sensorRange, trackStart == nil || sensorRange.lowerBound < trackStart! {
                let markerEnd = pendingText[sensorRange.upperBound...].firstIndex(where: { $0.isWhitespace }) ?? pendingText.endIndex
                let record = String(pendingText[..<markerEnd])
                pendingText.removeSubrange(..<markerEnd)
                events.append(contentsOf: parseRecord(record))
                continue
            }
            guard let match, let wholeRange = Range(match.range, in: pendingText) else { break }
            let prefixStart = pendingText.index(wholeRange.lowerBound,
                                                offsetBy: -min(512, pendingText.distance(from: pendingText.startIndex, to: wholeRange.lowerBound)))
            let record = String(pendingText[prefixStart..<wholeRange.upperBound])
            pendingText.removeSubrange(..<wholeRange.upperBound)
            events.append(contentsOf: parseRecord(record))
        }
        return events
    }

    mutating func consumeLine(_ line: String) -> [RadarLogEvent] {
        parseRecord(line)
    }

    private mutating func parseRecord(_ record: String) -> [RadarLogEvent] {
        let trimmed = record.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty else { return [] }

        if trimmed.range(of: "RADAR_HOME_STATE", options: [.caseInsensitive]) != nil {
            let home = homeState(in: trimmed)
            return [RadarLogEvent(source: .unknown, sourceReason: "home state", rawLine: trimmed,
                                  timestampMilliseconds: home.timestampMilliseconds, kind: .home(home))]
        }
        if trimmed.range(of: "RADAR_ROOM_STATE", options: [.caseInsensitive]) != nil {
            let identity = RadarSource.identify(in: trimmed)
            guard identity.source != .unknown else { return [] }
            let room = RadarRoomState(source: identity.source,
                                      roomID: RadarSource.fieldValue(in: trimmed, keys: ["room"]) ?? identity.source.defaultRoomID,
                                      occupied: booleanValue(in: trimmed, keys: ["occupied"]) ?? false,
                                      motion: RadarSource.fieldValue(in: trimmed, keys: ["motion"]) ?? "unknown",
                                      lastUpdateMilliseconds: timestampMilliseconds(in: trimmed))
            return [RadarLogEvent(source: identity.source, sourceReason: "room state", rawLine: trimmed,
                                  timestampMilliseconds: room.lastUpdateMilliseconds, kind: .room(room))]
        }

        let identity = RadarSource.identify(in: trimmed)
        let isRadarRx = trimmed.range(of: "RADAR_RX", options: [.caseInsensitive]) != nil ||
            trimmed.range(of: "radar_raw_rx", options: [.caseInsensitive]) != nil
        let isSourceScopedDiagnostic = isRadarRx ||
            trimmed.range(of: "parser[", options: [.caseInsensitive]) != nil ||
            trimmed.range(of: "recovery[", options: [.caseInsensitive]) != nil ||
            trimmed.range(of: "local track=", options: [.caseInsensitive]) != nil ||
            trimmed.range(of: "RADAR_PERSON_UPDATE", options: [.caseInsensitive]) != nil ||
            trimmed.range(of: "RADAR_COUNTS", options: [.caseInsensitive]) != nil ||
            trimmed.range(of: "PERSON_", options: [.caseInsensitive]) != nil
        let source: RadarSource
        let sourceReason: String
        if identity.source != .unknown {
            source = identity.source
            sourceReason = identity.reason
            mostRecentSource = source
        } else if !identity.reason.contains("conflict"), isSourceScopedDiagnostic {
            /* Only the legacy S3 UART grammar is allowed an implicit source.
             * Remote records must carry an explicit source/device pair so an
             * interleaved C51/C52 stream cannot overwrite the last room. */
            let isLegacyLocalMarker = trimmed.range(of: #"\b(?:local|radar_local|radar_uart|parser|recovery)\b"#, options: [.regularExpression, .caseInsensitive]) != nil
            let isLegacyLocal = isLegacyLocalMarker &&
                (mostRecentSource == nil || mostRecentSource == .s3Local)
            if isLegacyLocal {
                source = .s3Local
                sourceReason = "legacy S3 local source"
                mostRecentSource = .s3Local
            } else {
                source = .unknown
                sourceReason = "missing explicit source for remote diagnostic"
            }
        } else {
            source = identity.source
            sourceReason = identity.reason
        }
        let timestamp = timestampMilliseconds(in: trimmed)
        var patch = statePatch(in: trimmed)
        var events: [RadarLogEvent] = []

        let startsFrame = Self.legacyFrameMarker.firstMatch(
            in: trimmed,
            range: NSRange(trimmed.startIndex..., in: trimmed)
        ) != nil ||
            trimmed.range(of: "frame_start", options: [.caseInsensitive]) != nil
        if startsFrame {
            patch.isValidFrame = true
            events.append(RadarLogEvent(source: source,
                                        sourceReason: sourceReason,
                                        rawLine: trimmed,
                                        timestampMilliseconds: timestamp,
                                        kind: .frameStarted(patch)))
        }

        let range = NSRange(trimmed.startIndex..., in: trimmed)
        if let track = localTrack(in: trimmed, source: source) {
            var targetPatch = patch
            // A frame header and its per-target records commonly repeat the same sequence.
            // The header owns sequence de-duplication; individual targets must not reject each other.
            targetPatch.sequence = nil
            targetPatch.isValidFrame = true
            targetPatch.updatesTargetPosition = track.isVisible
            events.append(RadarLogEvent(source: source,
                                        sourceReason: sourceReason,
                                        rawLine: trimmed,
                                        timestampMilliseconds: timestamp,
                                        kind: .target(track, targetPatch)))
        } else if let target = acceptedTarget(in: trimmed, source: source) {
            var targetPatch = patch
            targetPatch.sequence = nil
            targetPatch.isValidFrame = true
            targetPatch.updatesTargetPosition = true
            events.append(RadarLogEvent(source: source,
                                        sourceReason: sourceReason,
                                        rawLine: trimmed,
                                        timestampMilliseconds: timestamp,
                                        kind: .target(target, targetPatch)))
        } else if let target = rawSlotTarget(in: trimmed, source: source) {
            var targetPatch = patch
            targetPatch.sequence = nil
            targetPatch.isValidFrame = true
            events.append(RadarLogEvent(source: source,
                                        sourceReason: sourceReason,
                                        rawLine: trimmed,
                                        timestampMilliseconds: timestamp,
                                        kind: .target(target, targetPatch)))
        } else if let match = Self.compactTrackPattern.firstMatch(in: trimmed, range: range),
                  let track = compactTarget(from: match, source: source, record: trimmed, timestamp: timestamp) {
            var targetPatch = patch
            targetPatch.sequence = nil
            targetPatch.isValidFrame = true
            targetPatch.updatesTargetPosition = true
            events.append(RadarLogEvent(source: source,
                                        sourceReason: sourceReason,
                                        rawLine: trimmed,
                                        timestampMilliseconds: timestamp,
                                        kind: .target(track, targetPatch)))
        } else if patch.hasValues && !startsFrame {
            events.append(RadarLogEvent(source: source,
                                        sourceReason: sourceReason,
                                        rawLine: trimmed,
                                        timestampMilliseconds: timestamp,
                                        kind: .update(patch)))
        } else if source == .unknown {
            // Keep unrecognized records visible even when no radar fields could be extracted.
            events.append(RadarLogEvent(source: .unknown,
                                        sourceReason: sourceReason,
                                        rawLine: trimmed,
                                        timestampMilliseconds: timestamp,
                                        kind: .update(RadarStatePatch())))
        }
        emitParseDiagnostic(for: trimmed, events: events)
        return events
    }

    private func localTrack(in record: String, source: RadarSource) -> RadarTarget? {
        guard record.range(of: #"\blocal\s+track="#, options: [.regularExpression, .caseInsensitive]) != nil,
              let trackID = integerValue(in: record, keys: ["track"]).flatMap(Int.init),
              let visible = integerValue(in: record, keys: ["visible"]).flatMap(Int.init),
              let rawX = integerValue(in: record, keys: ["raw_x"]).flatMap(Int.init),
              let rawY = integerValue(in: record, keys: ["raw_y"]).flatMap(Int.init) else { return nil }
        let filteredX = integerValue(in: record, keys: ["filtered_x"]).flatMap(Int.init)
        let filteredY = integerValue(in: record, keys: ["filtered_y"]).flatMap(Int.init)
        let x = filteredX ?? rawX
        let y = filteredY ?? rawY
        let distance = integerValue(in: record, keys: ["distance"]).flatMap(Int.init) ?? Self.distance(x: x, y: y)
        let angle = integerValue(in: record, keys: ["angle"]).flatMap(Int.init) ?? 0
        let speed = integerValue(in: record, keys: ["speed"]).flatMap(Int.init) ?? 0
        let direction = integerValue(in: record, keys: ["direction"]).flatMap(Int.init)
        let confidence = integerValue(in: record, keys: ["confidence"]).flatMap(Int.init) ?? 0
        let date = Date()
        return RadarTarget(source: source,
                           trackID: trackID,
                           xMillimeters: x,
                           yMillimeters: y,
                           rawXMillimeters: rawX,
                           rawYMillimeters: rawY,
                           filteredXMillimeters: filteredX,
                           filteredYMillimeters: filteredY,
                           distanceMillimeters: distance,
                           angleDegrees: angle,
                           speedCentimetersPerSecond: speed,
                           directionDegrees: direction,
                           confidence: confidence,
                           isVisible: visible == 1,
                           timestamp: date)
    }

    private func acceptedTarget(in record: String, source: RadarSource) -> RadarTarget? {
        guard record.range(of: #"\blocal\s+accepted\s+index="#, options: [.regularExpression, .caseInsensitive]) != nil,
              let trackID = integerValue(in: record, keys: ["index"]).flatMap(Int.init),
              let x = integerValue(in: record, keys: ["x"]).flatMap(Int.init),
              let y = integerValue(in: record, keys: ["y"]).flatMap(Int.init) else { return nil }
        return localTarget(source: source, trackID: trackID, x: x, y: y, record: record, isVisible: true)
    }

    private func rawSlotTarget(in record: String, source: RadarSource) -> RadarTarget? {
        guard record.range(of: #"\blocal\s+raw\s+slot="# , options: [.regularExpression, .caseInsensitive]) != nil,
              let trackID = integerValue(in: record, keys: ["slot"]).flatMap(Int.init),
              let x = integerValue(in: record, keys: ["x"]).flatMap(Int.init),
              let y = integerValue(in: record, keys: ["y"]).flatMap(Int.init) else { return nil }
        return localTarget(source: source, trackID: trackID, x: x, y: y, record: record, isVisible: false)
    }

    private func localTarget(source: RadarSource, trackID: Int, x: Int, y: Int, record: String, isVisible: Bool) -> RadarTarget {
        RadarTarget(source: source,
                    trackID: trackID,
                    xMillimeters: x,
                    yMillimeters: y,
                    rawXMillimeters: x,
                    rawYMillimeters: y,
                    distanceMillimeters: integerValue(in: record, keys: ["distance"]).flatMap(Int.init) ?? Self.distance(x: x, y: y),
                    angleDegrees: integerValue(in: record, keys: ["angle"]).flatMap(Int.init) ?? 0,
                    speedCentimetersPerSecond: integerValue(in: record, keys: ["speed"]).flatMap(Int.init) ?? 0,
                    directionDegrees: integerValue(in: record, keys: ["direction"]).flatMap(Int.init),
                    confidence: integerValue(in: record, keys: ["confidence"]).flatMap(Int.init) ?? 0,
                    isVisible: isVisible,
                    timestamp: Date())
    }

    private func compactTarget(from match: NSTextCheckingResult,
                               source: RadarSource,
                               record: String,
                               timestamp: Int64?) -> RadarTarget? {
        guard let x = integer(match, at: 2, record: record),
              let y = integer(match, at: 3, record: record) else { return nil }
        let trackID = integer(match, at: 1, record: record) ?? 0
        let distance = Int((Double(x) * Double(x) + Double(y) * Double(y)).squareRoot())
        return RadarTarget(source: source,
                           trackID: trackID,
                           xMillimeters: x,
                           yMillimeters: y,
                           rawXMillimeters: x,
                           rawYMillimeters: y,
                           distanceMillimeters: distance,
                           angleDegrees: 0,
                           speedCentimetersPerSecond: 0,
                           confidence: 0,
                           isVisible: true,
                           timestamp: Date())
    }

    private func statePatch(in text: String) -> RadarStatePatch {
        let sensor = RadarSource.fieldValue(in: text, keys: ["sensor"])
        let online = booleanValue(in: text, keys: ["online", "radar_online"]) ?? booleanValue(sensor)
        let connectionType = RadarSource.fieldValue(in: text, keys: ["connection_type", "link_type", "transport"])
        let sequence = integerValue(in: text, keys: ["frame_seq", "sequence", "seq"])
        let rawTargetCount = integerValue(in: text, keys: ["raw_target_count"]).flatMap(Int.init)
        let acceptedTargetCount = integerValue(in: text, keys: ["accepted_target_count"]).flatMap(Int.init)
        let visibleTrackCount = integerValue(in: text, keys: ["visible_track_count"]).flatMap(Int.init)
        let confirmedActiveTrackCount = integerValue(in: text, keys: ["confirmed_active_track_count"]).flatMap(Int.init)
        let historyTargetCount = integerValue(in: text, keys: ["history_target_count"]).flatMap(Int.init)
        let visiblePersonCount = integerValue(in: text, keys: ["visible_person_count"]).flatMap(Int.init)
        let retainedPersonCount = integerValue(in: text, keys: ["retained_person_count"]).flatMap(Int.init)
        let sourcePersonCount = integerValue(in: text, keys: ["source_person_count"]).flatMap(Int.init)
        let countState = RadarSource.fieldValue(in: text, keys: ["count_state"])
        let targetCount = visibleTrackCount ??
            integerValue(in: text, keys: ["tracks", "target_count", "targets"]).flatMap(Int.init)
        let occupancy = RadarSource.fieldValue(in: text, keys: ["occupancy"])
        let presence = RadarSource.fieldValue(in: text, keys: ["presence", "presence_state"])
            .flatMap(RadarPresenceState.init(rawValue:)) ?? occupancy.flatMap(RadarPresenceState.init(rawValue:))
        let motion = RadarSource.fieldValue(in: text, keys: ["motion", "motion_state"])
        let sensorState = RadarSource.fieldValue(in: text, keys: ["sensor_state"]) ?? sensor
        let spatial = RadarSource.fieldValue(in: text, keys: ["spatial", "spatial_state"]) ?? occupancy
        let acceptedFrames = integerValue(in: text, keys: ["accepted"]).flatMap(Int.init)
        let badHeader = integerValue(in: text, keys: ["bad_header"]).flatMap(Int.init)
        let badTail = integerValue(in: text, keys: ["bad_tail"]).flatMap(Int.init)
        let resyncCount = integerValue(in: text, keys: ["resync"]).flatMap(Int.init)
        let legacyParseErrors = integerValue(in: text, keys: ["parse_errors", "parser_errors", "parser_error"]).flatMap(Int.init)
        let parseErrors = legacyParseErrors ?? {
            guard badHeader != nil || badTail != nil else { return nil }
            return (badHeader ?? 0) + (badTail ?? 0)
        }()
        let sequenceRejects = integerValue(in: text, keys: ["sequence_rejects", "sequence_reject"]).flatMap(Int.init)
        let identityMismatches = integerValue(in: text, keys: ["identity_mismatches", "identity_mismatch"]).flatMap(Int.init)
        let droppedFrames = integerValue(in: text, keys: ["dropped_frames", "dropped_frame"]).flatMap(Int.init)
        let recoveryState = recoveryState(in: text)
        let isRadarRx = text.range(of: "RADAR_RX", options: [.caseInsensitive]) != nil ||
            text.range(of: "radar_raw_rx", options: [.caseInsensitive]) != nil
        let rxBytes = isRadarRx ? integerValue(in: text, keys: ["rx_bytes", "bytes"]).flatMap(Int.init) : nil
        let timeout = isRadarRx ? integerValue(in: text, keys: ["timeout"]).flatMap(Int.init) : nil
        let fifoOverflow = isRadarRx ? integerValue(in: text, keys: ["fifo_overflow", "fifo_overflow_count"]).flatMap(Int.init) : nil
        let isCountOrRoomUpdate = text.range(of: "RADAR_PERSON_UPDATE", options: [.caseInsensitive]) != nil ||
            text.range(of: "RADAR_COUNTS", options: [.caseInsensitive]) != nil ||
            text.range(of: "RADAR_ROOM_STATE", options: [.caseInsensitive]) != nil ||
            text.range(of: "RADAR_HOME_STATE", options: [.caseInsensitive]) != nil
        return RadarStatePatch(online: online,
                               connectionType: connectionType,
                               sequence: sequence,
                               targetCount: targetCount,
                               rawTargetCount: rawTargetCount,
                               acceptedTargetCount: acceptedTargetCount,
                               visibleTrackCount: visibleTrackCount,
                               confirmedActiveTrackCount: confirmedActiveTrackCount,
                               historyTargetCount: historyTargetCount,
                               visiblePersonCount: visiblePersonCount,
                               retainedPersonCount: retainedPersonCount,
                               sourcePersonCount: sourcePersonCount,
                               countState: countState,
                               presenceState: presence,
                               motionState: motion,
                               spatialState: spatial,
                               parserErrors: parseErrors,
                               sequenceRejects: sequenceRejects,
                               identityMismatches: identityMismatches,
                               droppedFrames: droppedFrames,
                               recoveryState: recoveryState,
                               deviceID: RadarSource.fieldValue(in: text, keys: ["device_id", "device", "did"]),
                               roomID: RadarSource.fieldValue(in: text, keys: ["room_id", "room"]),
                               sensorState: sensorState,
                               occupancyState: occupancy,
                               acceptedFrames: acceptedFrames,
                               badHeader: badHeader,
                               badTail: badTail,
                               resyncCount: resyncCount,
                               rxBytes: rxBytes,
                               timeout: timeout,
                               fifoOverflow: fifoOverflow,
                               isValidFrame: targetCount != nil && !isCountOrRoomUpdate)
    }

    private func homeState(in text: String) -> RadarHomeState {
        let timestamp = timestampMilliseconds(in: text)
        let declaredCount = integerValue(in: text, keys: ["occupied_room_count"]).flatMap(Int.init) ?? 0
        let homePersonCount = integerValue(in: text, keys: ["home_person_count"]).flatMap(Int.init) ?? 0
        let marker = "occupied_rooms=["
        guard let start = text.range(of: marker, options: [.caseInsensitive]),
              let end = text[start.upperBound...].firstIndex(of: "]") else {
            return RadarHomeState(occupiedRoomCount: declaredCount, homePersonCount: homePersonCount,
                                  timestampMilliseconds: timestamp)
        }
        let rooms = text[start.upperBound..<end].split(separator: "|").compactMap { item -> RadarRoomState? in
            let parts = item.split(separator: ":", maxSplits: 1).map(String.init)
            guard parts.count == 2, let source = RadarSource.source(named: parts[0]) else { return nil }
            return RadarRoomState(source: source, roomID: parts[1], occupied: true,
                                  motion: "unknown", lastUpdateMilliseconds: timestamp)
        }
        return RadarHomeState(occupiedRoomCount: declaredCount, occupiedRooms: rooms,
                              homePersonCount: homePersonCount, timestampMilliseconds: timestamp)
    }

    private func recoveryState(in text: String) -> String? {
        if let rawState = bracketedValue(in: text, section: "recovery", key: "state")?.uppercased() {
            switch rawState {
            case "RUNNING": return "normal"
            case "WAITING_VALID": return "recovering"
            case "BACKOFF": return "fault_recovering"
            case "FAILED": return "error"
            default: return rawState.lowercased()
            }
        }
        if RadarSource.fieldValue(in: text, keys: ["recovery"])?.lowercased() == "complete" {
            return "recovered"
        }
        if text.range(of: "reconnect", options: [.caseInsensitive]) != nil {
            return "reconnecting"
        } else if text.range(of: "c5.*report", options: [.regularExpression, .caseInsensitive]) != nil {
            return "c5_reporting"
        } else {
            return nil
        }
    }

    private func bracketedValue(in text: String, section: String, key: String) -> String? {
        let escapedSection = NSRegularExpression.escapedPattern(for: section)
        let escapedKey = NSRegularExpression.escapedPattern(for: key)
        let pattern = "\(escapedSection)\\s*\\[\\s*\(escapedKey)\\s*=\\s*([^\\s\\]]+)"
        guard let expression = try? NSRegularExpression(pattern: pattern, options: [.caseInsensitive]),
              let match = expression.firstMatch(in: text, range: NSRange(text.startIndex..., in: text)),
              let range = Range(match.range(at: 1), in: text) else { return nil }
        return String(text[range])
    }

    private func timestampMilliseconds(in text: String) -> Int64? {
        if let value = integerValue(in: text, keys: ["timestamp_ms", "ts_ms", "time_ms", "timestamp"]) {
            return value
        }
        let pattern = #"\((\d{1,13})\)"#
        guard let expression = try? NSRegularExpression(pattern: pattern),
              let match = expression.firstMatch(in: text, range: NSRange(text.startIndex..., in: text)),
              let range = Range(match.range(at: 1), in: text) else { return nil }
        return Int64(text[range])
    }

    private func integerValue(in text: String, keys: [String]) -> Int64? {
        RadarSource.fieldValue(in: text, keys: keys).flatMap(Int64.init)
    }

    private func booleanValue(in text: String, keys: [String]) -> Bool? {
        booleanValue(RadarSource.fieldValue(in: text, keys: keys))
    }

    private func booleanValue(_ value: String?) -> Bool? {
        guard let value = value?.lowercased() else { return nil }
        switch value {
        case "1", "true", "online", "up", "connected": return true
        case "0", "false", "offline", "down", "disconnected": return false
        default: return nil
        }
    }

    private func integer(_ match: NSTextCheckingResult, at index: Int, record: String) -> Int? {
        guard let range = Range(match.range(at: index), in: record) else { return nil }
        return Int(record[range])
    }

    private static func distance(x: Int, y: Int) -> Int {
        Int((Double(x) * Double(x) + Double(y) * Double(y)).squareRoot())
    }

    private func emitParseDiagnostic(for record: String, events: [RadarLogEvent]) {
        let localCoordinateRecord = record.range(of: #"\blocal\s+(?:track|accepted|raw)\b"#, options: [.regularExpression, .caseInsensitive]) != nil
        guard localCoordinateRecord else { return }
        if let target = events.compactMap({ event -> RadarTarget? in
            if case let .target(target, _) = event.kind { return target }
            return nil
        }).first {
            print("RADAR_PARSE_OK track=\(target.trackID) x=\(target.xMillimeters) y=\(target.yMillimeters) distance=\(target.distanceMillimeters) confidence=\(target.confidence)")
        } else {
            print("RADAR_PARSE_FAIL line=\(sanitized(record))")
        }
    }

    private func sanitized(_ record: String) -> String {
        let withoutDeviceID = record.replacingOccurrences(of: #"(?i)(device_id|device|did)=([^\s,]+)"#, with: "$1=<redacted>", options: .regularExpression)
        return String(withoutDeviceID.prefix(240))
    }
}
