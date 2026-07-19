import Foundation

@main
enum ParserChecks {
    static var failures = 0

    static func expect(_ condition: @autoclosure () -> Bool, _ message: String) {
        guard condition() else {
            fputs("FAIL: \(message)\n", stderr)
            failures += 1
            return
        }
        print("PASS: \(message)")
    }

    static func main() async {
        testSourceRecognition()
        testUnknownIsolation()
        testInterleavedFixture()
        testOfflineAndRecoveryPreserveLastValidFrame()
        testRadarRxUsesMostRecentSource()
        testSequenceAndTracks()
        testRoomIsolation()
        testLegacyCompatibility()
        testCurrentS3LocalTrackCompatibility()
        testVisibleZeroKeepsLastAcceptedPosition()
        testAcceptedAndRawSlotCompatibility()
        testLocalHealthPreservesLastTarget()
        testRelativeLogTimestampUsesReceiveTime()
        testReplayRestart()

        if failures == 0 {
            print("PASS: multi-source radar offline checks")
        } else {
            fputs("FAIL: \(failures) multi-source radar checks failed\n", stderr)
            exit(1)
        }
    }

    static func testSourceRecognition() {
        expect(RadarSource.identify(in: "source=S3_LOCAL local_source=2").source == .s3Local, "explicit source wins")
        expect(RadarSource.identify(in: "source=S3").source == .s3Local, "S3 alias maps to S3_LOCAL")
        expect(RadarSource.identify(in: "source=0").source == .s3Local, "numeric S3 source maps")
        expect(RadarSource.identify(in: "source=ESPC51").source == .c51, "ESPC51 alias maps")
        expect(RadarSource.identify(in: "source=1").source == .c51, "numeric C51 source maps")
        expect(RadarSource.identify(in: "source=ESPC52").source == .c52, "ESPC52 alias maps")
        expect(RadarSource.identify(in: "source=2").source == .c52, "numeric C52 source maps")
        expect(RadarSource.identify(in: "device_id=sensair_s3_gateway_01").source == .s3Local, "S3 device id maps")
        expect(RadarSource.identify(in: "device_id=sensair_shuttle_01").source == .c51, "C51 device id maps")
        expect(RadarSource.identify(in: "local_id=sensair_shuttle_02").source == .c52, "C52 local id maps")
    }

    static func testUnknownIsolation() {
        var store = RadarStateStore()
        store.apply(event(source: .unknown, trackID: 9, sequence: 1, raw: "source=unmapped track"), nowMilliseconds: 1_000)
        expect(RadarSource.roomSources.allSatisfy { store.states[$0]?.filteredTargets.isEmpty == true }, "UNKNOWN does not add room targets")
        expect(store.unknownDiagnostics.count == 1, "UNKNOWN enters diagnostics")
    }

    static func testInterleavedFixture() {
        guard CommandLine.arguments.count > 1,
              let text = try? String(contentsOfFile: CommandLine.arguments[1], encoding: .utf8) else {
            expect(false, "mixed fixture is readable")
            return
        }
        var parser = RadarLogParser()
        var store = RadarStateStore()
        for line in text.split(whereSeparator: \.isNewline) {
            parser.consumeLine(String(line)).forEach { store.apply($0, nowMilliseconds: 2_000) }
        }
        expect(store.states[.s3Local]?.targetCount == 1, "S3 panel contains only one S3 target")
        expect(store.states[.c51]?.targetCount == 1, "C51 panel contains only one C51 target")
        expect(store.states[.c52]?.targetCount == 3, "C52 panel contains only three C52 targets")
        expect(store.unknownDiagnostics.count == 1, "fixture UNKNOWN stays outside room panels")
        expect(store.states[.c51]?.recoveryState == "recovered", "C51 reconnect/recovery is isolated")
        expect(store.states[.c52]?.parseErrorCount == 1, "C52 parser error stays isolated")
        expect(store.states[.s3Local]?.sourceId == 0 && store.states[.s3Local]?.deviceId == "sensair_s3_gateway_01", "S3 source identity is visible")
        expect(store.states[.s3Local]?.online == true && store.states[.s3Local]?.occupancyState == "occupied", "S3 sensor and occupancy parse")
        expect(store.states[.s3Local]?.acceptedFrames == 3_200 && store.states[.s3Local]?.badHeader == 10 && store.states[.s3Local]?.badTail == 2 && store.states[.s3Local]?.resyncCount == 1, "S3 parser health parses independently")
        expect(store.states[.s3Local]?.parseErrorCount == 12 && store.states[.s3Local]?.recoveryState == "recovering", "new parser errors and recovery state map correctly")
        expect(store.states[.s3Local]?.uartHealth.rxBytes == 30 && store.states[.s3Local]?.uartHealth.timeout == 4 && store.states[.s3Local]?.uartHealth.fifoOverflow == 1, "RADAR_RX belongs to the most recent S3 source")
    }

    static func testOfflineAndRecoveryPreserveLastValidFrame() {
        var parser = RadarLogParser()
        var store = RadarStateStore()
        parser.consumeLine("source=S3_LOCAL sensor=online occupancy=occupied tracks=1").forEach { store.apply($0, nowMilliseconds: 1_000) }
        parser.consumeLine("local track=x:120 y:50").forEach { store.apply($0, nowMilliseconds: 1_001) }
        let validTargets = store.states[.s3Local]?.lastValidTargets
        let validTimestamp = store.states[.s3Local]?.lastValidTimestamp

        parser.consumeLine("source=S3_LOCAL sensor=offline").forEach { store.apply($0, nowMilliseconds: 2_000) }
        parser.consumeLine("recovery[state=BACKOFF]").forEach { store.apply($0, nowMilliseconds: 2_001) }

        expect(store.states[.s3Local]?.online == false, "offline maps to online false")
        expect(store.states[.s3Local]?.lastValidTargets == validTargets, "offline and recovery do not clear valid targets")
        expect(store.states[.s3Local]?.lastValidTimestamp == validTimestamp, "offline and recovery do not refresh last valid timestamp")
        expect(store.states[.s3Local]?.recoveryState == "fault_recovering", "BACKOFF does not report recovered")
    }

    static func testRadarRxUsesMostRecentSource() {
        var parser = RadarLogParser()
        var store = RadarStateStore()
        _ = parser.consumeLine("source=C52 sensor=online")
        let events = parser.consumeLine("radar_raw_rx bytes=48 timeout=2 fifo_overflow=3")
        expect(events.allSatisfy { $0.source == .c52 }, "RADAR_RX resolves to the most recent source")
        events.forEach { store.apply($0, nowMilliseconds: 1_000) }
        expect(store.states[.c52]?.uartHealth.rxBytes == 48 && store.states[.c52]?.uartHealth.timeout == 2 && store.states[.c52]?.uartHealth.fifoOverflow == 3, "RADAR_RX updates only C52 UART health")
        expect(store.states[.s3Local]?.uartHealth.rxBytes == 0 && store.states[.c51]?.uartHealth.rxBytes == 0, "RADAR_RX does not leak across sources")
    }

    static func testSequenceAndTracks() {
        var store = RadarStateStore()
        store.apply(event(source: .c51, trackID: 1, sequence: 7, x: -1_000), nowMilliseconds: 1_000)
        store.apply(event(source: .c52, trackID: 1, sequence: 7, x: 2_000), nowMilliseconds: 1_001)
        expect(store.states[.c51]?.sequenceRejectCount == 0 && store.states[.c52]?.sequenceRejectCount == 0, "same sequence is valid across sources")
        expect(store.states[.c51]?.trackHistory[1]?.first?.xMillimeters == -1_000, "C51 tracker history is independent")
        expect(store.states[.c52]?.trackHistory[1]?.first?.xMillimeters == 2_000, "C52 tracker history is independent")

        store.apply(event(source: .c51, trackID: 2, sequence: 7), nowMilliseconds: 1_002)
        expect(store.states[.c51]?.sequenceRejectCount == 1, "duplicate sequence is rejected only within C51")
        expect(store.states[.c51]?.filteredTargets.map(\.trackID) == [1], "rejected frame does not replace C51 target")
    }

    static func testRoomIsolation() {
        var store = RadarStateStore()
        for source in RadarSource.roomSources { store.apply(event(source: source, trackID: 1, sequence: 1), nowMilliseconds: 1_000) }
        store.clearTracks(for: .c51)
        expect(store.states[.c51]?.trackHistory.isEmpty == true, "clear C51 clears only C51 trails")
        expect(store.states[.s3Local]?.trackHistory.isEmpty == false && store.states[.c52]?.trackHistory.isEmpty == false, "clear C51 leaves S3 and C52 trails")

        var c51Config = RadarRoomConfig.default(for: .c51)
        c51Config.roomName = "Kitchen"
        c51Config.coordinateConfig.maximumXMillimeters = 9_000
        store.setConfig(c51Config, for: .c51)
        expect(store.states[.c51]?.coordinateConfig.maximumXMillimeters == 9_000, "C51 coordinate config changes")
        expect(store.states[.c52]?.coordinateConfig.maximumXMillimeters == 6_000 && store.states[.s3Local]?.roomName == RadarSource.s3Local.defaultRoomName, "coordinate configs remain per source")

        store.apply(event(source: .c52, trackID: 2, sequence: 2), nowMilliseconds: 0)
        store.apply(event(source: .s3Local, trackID: 2, sequence: 2), nowMilliseconds: 4_000)
        store.apply(event(source: .c51, trackID: 2, sequence: 2), nowMilliseconds: 4_000)
        store.refreshFreshness(nowMilliseconds: 4_001)
        expect(store.states[.c52]?.freshnessState == .stale, "C52 transitions stale")
        expect(store.states[.s3Local]?.freshnessState == .fresh && store.states[.c51]?.freshnessState == .fresh, "C52 stale does not affect S3/C51")
        expect(RadarDashboardLayout.panels(visibleSource: nil) == [.s3Local, .c51, .c52], "desktop layout renders three panels")
        expect(RadarDashboardLayout.panels(visibleSource: .c51) == [.c51] && store.states[.s3Local]?.targetCount != nil, "narrow tab/filter preserves background state")
    }

    static func testLegacyCompatibility() {
        var parser = ESPS3RadarLogParser()
        let record = "I radar_diag: local sensor=valid local track=1 visible=1 raw_x=10 raw_y=20 filtered_x=30 filtered_y=40 distance=50 angle=60 speed=7 direction=60 confidence=8 seen=1 missed=0"
        let events = parser.consume(Array(record.utf8))
        expect(events.count == 2 && events.first == .snapshotStarted, "single-source legacy log remains compatible")

        let compatRecord = "I radar_log: RADAR_TRACK_COMPAT: local track=2 visible=1 raw_x=-1846 raw_y=2273 filtered_x=-1846 filtered_y=2273 distance=2928 angle=-39 speed=0 direction=0 confidence=80 seen=4 missed=0"
        let compatEvents = parser.consume(Array(compatRecord.utf8))
        let compatTrack = compatEvents.compactMap { event -> S3RadarTrack? in
            if case let .track(track) = event { return track }
            return nil
        }.first
        expect(compatTrack?.trackID == 2 && compatTrack?.xMillimeters == -1_846 &&
               compatTrack?.yMillimeters == 2_273 && compatTrack?.isVisible == true,
               "RADAR_TRACK_COMPAT renders its millimeter coordinates")

        var state = RadarStateStore()
        state.apply(event(source: .c51, trackID: 4, sequence: 1), nowMilliseconds: 1_000)
        state.apply(RadarLogEvent(source: .c51, sourceReason: "explicit", rawLine: "source=C51 online=1", timestampMilliseconds: 1_100, kind: .update(RadarStatePatch(online: true))), nowMilliseconds: 1_100)
        expect(state.states[.c51]?.filteredTargets.map(\.trackID) == [4], "missing fields do not clear valid state")
    }

    @MainActor static func testCurrentS3LocalTrackCompatibility() {
        guard CommandLine.arguments.count > 2,
              let text = try? String(contentsOfFile: CommandLine.arguments[2], encoding: .utf8) else {
            expect(false, "current S3 local fixture is readable")
            return
        }
        var parser = RadarLogParser()
        var store = RadarStateStore()
        let replay = RadarReplayController()
        replay.load(text)
        _ = replay.step { line in
            parser.consumeLine(line).forEach { store.apply($0, nowMilliseconds: 1_000) }
        }
        let target = store.states[.s3Local]?.filteredTargets.first
        let importedRecordCount = replay.recordCount
        expect(importedRecordCount == 2, "current S3 local log imports as a two-record replay")
        expect(target?.xMillimeters == -1_427 && target?.yMillimeters == 4_594, "S3 local track prefers filtered_x and filtered_y")
        expect(target?.distanceMillimeters == 5_131 && target?.isVisible == true, "S3 local track exposes distance and visible")
        expect(target?.source == .s3Local && target?.directionDegrees == 40 && target?.confidence == 80, "S3 local track keeps source, direction, and confidence")
    }

    static func testVisibleZeroKeepsLastAcceptedPosition() {
        var parser = RadarLogParser()
        var store = RadarStateStore()
        let visible = "local track=4 visible=1 raw_x=-1074 raw_y=5017 filtered_x=-1427 filtered_y=4594 distance=5131 angle=-12 speed=8 direction=40 confidence=80"
        let hidden = "local track=4 visible=0 raw_x=-1074 raw_y=5017 filtered_x=-1427 filtered_y=4594 distance=5131 angle=-12 speed=8 direction=40 confidence=0"
        parser.consumeLine(visible).forEach { store.apply($0, nowMilliseconds: 1_000) }
        parser.consumeLine(hidden).forEach { store.apply($0, nowMilliseconds: 2_000) }
        let target = store.states[.s3Local]?.filteredTargets.first
        expect(target?.xMillimeters == -1_427 && target?.yMillimeters == 4_594 && target?.isVisible == false, "visible=0 keeps the last accepted position and marks it stale")
        expect(target?.confidence == 0 && target?.lastSeenTime.timeIntervalSince1970 ?? 0 > 0, "visible=0 refreshes telemetry without using a log timestamp")
    }

    static func testAcceptedAndRawSlotCompatibility() {
        var parser = RadarLogParser()
        var store = RadarStateStore()
        parser.consumeLine("local accepted index=0 x=-1074 y=5017 distance=5131 angle=-12 speed=8").forEach { store.apply($0, nowMilliseconds: 1_000) }
        parser.consumeLine("local raw slot=0 x=20 y=30 speed=8").forEach { store.apply($0, nowMilliseconds: 2_000) }
        let target = store.states[.s3Local]?.filteredTargets.first
        expect(target?.xMillimeters == -1_074 && target?.yMillimeters == 5_017 && target?.isVisible == false, "accepted position survives a raw-slot diagnostic")
    }

    static func testLocalHealthPreservesLastTarget() {
        var parser = RadarLogParser()
        var store = RadarStateStore()
        parser.consumeLine("local accepted index=0 x=-1074 y=5017 distance=5131 angle=-12 speed=8").forEach { store.apply($0, nowMilliseconds: 1_000) }
        parser.consumeLine("local sensor=valid occupancy=present motion=moving").forEach { store.apply($0, nowMilliseconds: 2_000) }
        parser.consumeLine("local sensor=offline occupancy=unknown").forEach { store.apply($0, nowMilliseconds: 3_000) }
        expect(store.states[.s3Local]?.filteredTargets.first?.xMillimeters == -1_074, "sensor health and offline reports do not clear the last accepted target")
        expect(store.states[.s3Local]?.online == false && store.states[.s3Local]?.occupancyState == "unknown", "local sensor health still updates independently")

        let malformed = parser.consumeLine("local track=4 visible=1 raw_x=-1074 filtered_x=-1427")
        expect(malformed.isEmpty, "malformed local coordinate record is rejected and logged as RADAR_PARSE_FAIL")
    }

    static func testRelativeLogTimestampUsesReceiveTime() {
        var parser = RadarLogParser()
        var store = RadarStateStore()
        let line = "I (1000) radar_ble: source=C51 online=1 frame_seq=9 target_count=0"
        parser.consumeLine(line).forEach { store.apply($0, nowMilliseconds: 9_000) }
        expect(store.states[.c51]?.lastUpdateMilliseconds == 9_000, "boot-relative log timestamp does not create false stale data")
    }

    @MainActor static func testReplayRestart() {
        guard CommandLine.arguments.count > 1,
              let text = try? String(contentsOfFile: CommandLine.arguments[1], encoding: .utf8) else { return }
        let replay = RadarReplayController()
        replay.load(text)
        var parser = RadarLogParser()
        var store = RadarStateStore()
        while replay.step(process: { line in parser.consumeLine(line).forEach { store.apply($0, nowMilliseconds: 2_000) } }) {}
        expect(store.states[.c52]?.targetCount == 3 && store.unknownDiagnostics.count == 1, "replay processes all interleaved sources")
        replay.restart { store.resetRuntimeState(); parser = RadarLogParser() }
        expect(RadarSource.roomSources.allSatisfy { store.states[$0]?.targetCount == 0 } && store.unknownDiagnostics.isEmpty, "replay restart resets rooms and UNKNOWN")
    }

    static func event(source: RadarSource, trackID: Int, sequence: Int64, x: Int = 100, raw: String = "") -> RadarLogEvent {
        let target = RadarTarget(trackID: trackID,
                                 xMillimeters: x,
                                 yMillimeters: 1_000,
                                 rawXMillimeters: x,
                                 rawYMillimeters: 1_000,
                                 distanceMillimeters: 1_005,
                                 angleDegrees: 5,
                                 speedCentimetersPerSecond: 10,
                                 confidence: 80,
                                 isVisible: true,
                                 timestamp: .now)
        return RadarLogEvent(source: source,
                             sourceReason: "test",
                             rawLine: raw,
                             timestampMilliseconds: nil,
                             kind: .target(target, RadarStatePatch(sequence: sequence, updatesTargetPosition: true)))
    }
}
