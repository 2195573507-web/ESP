import Foundation

struct RadarStateStore {
    private(set) var states: [RadarSource: RadarRoomState]
    private(set) var unknownDiagnostics: [UnknownRadarDiagnostic] = []
    private let freshnessPolicy: RadarFreshnessPolicy
    private let maximumUnknownDiagnostics = 200

    init(configs: [RadarSource: RadarRoomConfig] = [:],
         freshnessPolicy: RadarFreshnessPolicy = .default) {
        self.freshnessPolicy = freshnessPolicy
        states = Dictionary(uniqueKeysWithValues: RadarSource.allCases.map { source in
            (source, RadarRoomState(source: source, config: configs[source] ?? .default(for: source)))
        })
    }

    mutating func apply(_ event: RadarLogEvent, nowMilliseconds: Int64 = RadarClock.nowMilliseconds) {
        // The app receive time, never an ESP log timestamp, owns target freshness.
        let eventTime = nowMilliseconds
        guard event.source != .unknown else {
            appendUnknown(event, timestampMilliseconds: eventTime)
            return
        }
        guard var state = states[event.source] else { return }

        switch event.kind {
        case .frameStarted(let patch):
            var framePatch = patch
            framePatch.isValidFrame = true
            guard apply(patch: framePatch, to: &state, eventTime: eventTime) else { break }
        case .target(let target, let patch):
            var targetPatch = patch
            targetPatch.isValidFrame = true
            guard apply(patch: targetPatch, to: &state, eventTime: eventTime) else { break }
            upsert(target: target, updatesPosition: targetPatch.updatesTargetPosition, into: &state, eventTime: eventTime)
            state.lastValidTargets = state.filteredTargets
        case .update(let patch):
            _ = apply(patch: patch, to: &state, eventTime: eventTime)
        }
        states[event.source] = state
    }

    mutating func clearTracks(for source: RadarSource) {
        guard var state = states[source] else { return }
        state.trackHistory.removeAll()
        state.tracks.removeAll()
        state.targetDisappearanceMilliseconds.removeAll()
        states[source] = state
    }

    mutating func resetRuntimeState() {
        for source in RadarSource.allCases {
            let config = currentConfig(for: source)
            states[source] = RadarRoomState(source: source, config: config)
        }
        unknownDiagnostics.removeAll()
    }

    mutating func setConfig(_ config: RadarRoomConfig, for source: RadarSource) {
        guard var state = states[source] else { return }
        state.roomName = config.roomName
        state.coordinateConfig = config.coordinateConfig
        state.zoneConfig = config.zoneConfig
        states[source] = state
    }

    mutating func refreshFreshness(nowMilliseconds: Int64 = RadarClock.nowMilliseconds) {
        for source in RadarSource.roomSources {
            guard var state = states[source] else { continue }
            let nextState: RadarFreshnessState
            if let last = state.lastValidTimestamp {
                let age = max(0, nowMilliseconds - last)
                if age <= freshnessPolicy.freshAfterMilliseconds {
                    nextState = .fresh
                } else if age <= freshnessPolicy.offlineAfterMilliseconds {
                    nextState = .stale
                } else {
                    nextState = .offline
                }
            } else {
                nextState = .neverSeen
            }
            if state.freshnessState != .stale && nextState == .stale {
                state.staleFrameCount += 1
            }
            state.freshnessState = nextState
            if nextState == .offline || nextState == .neverSeen {
                state.online = false
            }
            states[source] = state
        }
    }

    private func currentConfig(for source: RadarSource) -> RadarRoomConfig {
        guard let state = states[source] else { return .default(for: source) }
        return RadarRoomConfig(roomName: state.roomName,
                               coordinateConfig: state.coordinateConfig,
                               zoneConfig: state.zoneConfig)
    }

    private mutating func appendUnknown(_ event: RadarLogEvent, timestampMilliseconds: Int64) {
        let identity = RadarSource.identify(in: event.rawLine)
        unknownDiagnostics.append(UnknownRadarDiagnostic(rawSummary: String(event.rawLine.prefix(240)),
                                                         reason: event.sourceReason,
                                                         candidateSource: identity.candidate,
                                                         candidateDeviceID: identity.deviceID,
                                                         timestampMilliseconds: timestampMilliseconds))
        if unknownDiagnostics.count > maximumUnknownDiagnostics {
            unknownDiagnostics.removeFirst(unknownDiagnostics.count - maximumUnknownDiagnostics)
        }
        guard var state = states[.unknown] else { return }
        state.lastUpdateMilliseconds = timestampMilliseconds
        state.parseErrorCount += 1
        state.freshnessState = .fresh
        states[.unknown] = state
    }

    private func apply(patch: RadarStatePatch,
                       to state: inout RadarRoomState,
                       eventTime: Int64) -> Bool {
        applyHealth(patch: patch, to: &state)
        guard patch.isValidFrame else { return true }

        if let sequence = patch.sequence {
            if let previousSequence = state.lastSequence, sequence <= previousSequence {
                state.sequenceRejectCount += 1
                return false
            }
            state.lastSequence = sequence
        }

        if let previousTime = state.lastValidTimestamp, eventTime > previousTime {
            let delta = eventTime - previousTime
            if delta > 0 { state.frameRate = 1_000.0 / Double(delta) }
        }
        state.lastUpdateMilliseconds = eventTime
        state.lastValidTimestamp = eventTime
        state.freshnessState = .fresh
        state.online = patch.online ?? true
        if let targetCount = patch.targetCount { state.targetCount = targetCount }
        if let presenceState = patch.presenceState { state.presenceState = presenceState }
        if let motionState = patch.motionState { state.motionState = motionState }
        if let spatialState = patch.spatialState { state.spatialState = spatialState }
        return true
    }

    private func applyHealth(patch: RadarStatePatch, to state: inout RadarRoomState) {
        if let online = patch.online { state.online = online }
        if let connectionType = patch.connectionType { state.connectionType = connectionType }
        if let deviceID = patch.deviceID, !deviceID.isEmpty { state.deviceId = deviceID }
        if let sensorState = patch.sensorState { state.sensorState = sensorState }
        if let occupancyState = patch.occupancyState { state.occupancyState = occupancyState }
        if let parserErrors = patch.parserErrors { state.parseErrorCount = max(state.parseErrorCount, parserErrors) }
        if let sequenceRejects = patch.sequenceRejects { state.sequenceRejectCount = max(state.sequenceRejectCount, sequenceRejects) }
        if let identityMismatches = patch.identityMismatches { state.identityMismatchCount = max(state.identityMismatchCount, identityMismatches) }
        if let droppedFrames = patch.droppedFrames { state.droppedFrameCount = max(state.droppedFrameCount, droppedFrames) }
        if let recoveryState = patch.recoveryState { state.recoveryState = recoveryState }
        if let acceptedFrames = patch.acceptedFrames {
            state.parserHealth.acceptedFrames = max(state.parserHealth.acceptedFrames, acceptedFrames)
        }
        if let badHeader = patch.badHeader {
            state.parserHealth.badHeader = max(state.parserHealth.badHeader, badHeader)
        }
        if let badTail = patch.badTail {
            state.parserHealth.badTail = max(state.parserHealth.badTail, badTail)
        }
        if let resyncCount = patch.resyncCount {
            state.parserHealth.resyncCount = max(state.parserHealth.resyncCount, resyncCount)
        }
        if let rxBytes = patch.rxBytes { state.uartHealth.rxBytes = max(state.uartHealth.rxBytes, rxBytes) }
        if let timeout = patch.timeout { state.uartHealth.timeout = max(state.uartHealth.timeout, timeout) }
        if let fifoOverflow = patch.fifoOverflow {
            state.uartHealth.fifoOverflow = max(state.uartHealth.fifoOverflow, fifoOverflow)
        }
        state.lastParserHealth = state.parserHealth
    }

    private func upsert(target: RadarTarget,
                        updatesPosition: Bool,
                        into state: inout RadarRoomState,
                        eventTime: Int64) {
        guard updatesPosition else {
            // A disappearance/raw-slot report must never overwrite or delete the last accepted position.
            guard let previous = state.filteredTargets.first(where: { $0.trackID == target.trackID }) else { return }
            let stale = RadarTarget(source: target.source,
                                    trackID: target.trackID,
                                    xMillimeters: previous.xMillimeters,
                                    yMillimeters: previous.yMillimeters,
                                    rawXMillimeters: target.rawXMillimeters ?? previous.rawXMillimeters,
                                    rawYMillimeters: target.rawYMillimeters ?? previous.rawYMillimeters,
                                    filteredXMillimeters: target.filteredXMillimeters ?? previous.filteredXMillimeters,
                                    filteredYMillimeters: target.filteredYMillimeters ?? previous.filteredYMillimeters,
                                    distanceMillimeters: target.distanceMillimeters,
                                    angleDegrees: target.angleDegrees,
                                    speedCentimetersPerSecond: target.speedCentimetersPerSecond,
                                    directionDegrees: target.directionDegrees ?? previous.directionDegrees,
                                    confidence: target.confidence,
                                    isVisible: false,
                                    timestamp: Date(),
                                    lastSeenTime: previous.lastSeenTime)
            replace(stale, in: &state.rawTargets)
            replace(stale, in: &state.filteredTargets)
            state.tracks[target.trackID] = RadarTrack(trackerID: target.trackID,
                                                      latestTarget: stale,
                                                      lastSeenMs: Int64(stale.lastSeenTime.timeIntervalSince1970 * 1_000))
            state.targetCount = state.filteredTargets.filter(\.isVisible).count
            state.stableTargetCount = state.targetCount
            return
        }

        let accepted = RadarTarget(source: target.source,
                                   trackID: target.trackID,
                                   xMillimeters: target.xMillimeters,
                                   yMillimeters: target.yMillimeters,
                                   rawXMillimeters: target.rawXMillimeters,
                                   rawYMillimeters: target.rawYMillimeters,
                                   filteredXMillimeters: target.filteredXMillimeters,
                                   filteredYMillimeters: target.filteredYMillimeters,
                                   distanceMillimeters: target.distanceMillimeters,
                                   angleDegrees: target.angleDegrees,
                                   speedCentimetersPerSecond: target.speedCentimetersPerSecond,
                                   directionDegrees: target.directionDegrees,
                                   confidence: target.confidence,
                                   isVisible: true,
                                   timestamp: Date())
        replace(accepted, in: &state.rawTargets)
        replace(accepted, in: &state.filteredTargets)
        state.tracks[accepted.trackID] = RadarTrack(trackerID: accepted.trackID,
                                                    latestTarget: accepted,
                                                    lastSeenMs: eventTime)
        state.trackHistory[accepted.trackID, default: []].append(RadarTrackPoint(xMillimeters: accepted.xMillimeters,
                                                                                   yMillimeters: accepted.yMillimeters,
                                                                                   timestampMs: eventTime))
        let cutoff = eventTime - state.coordinateConfig.trackRetentionMilliseconds
        state.trackHistory[accepted.trackID]?.removeAll { $0.timestampMs < cutoff }
        state.targetDisappearanceMilliseconds[accepted.trackID] = eventTime
        state.targetCount = state.filteredTargets.filter(\.isVisible).count
        state.stableTargetCount = state.targetCount
        state.lastValidTargets = state.filteredTargets
        if state.targetCount > 0 && state.presenceState == .unknown {
            state.presenceState = accepted.speedCentimetersPerSecond == 0 ? .hold : .motion
        }
        if state.motionState == "unknown" {
            state.motionState = accepted.speedCentimetersPerSecond == 0 ? "hold" : "motion"
        }
    }

    private func replace(_ target: RadarTarget, in targets: inout [RadarTarget]) {
        if let index = targets.firstIndex(where: { $0.trackID == target.trackID }) {
            targets[index] = target
        } else {
            targets.append(target)
            targets.sort { $0.trackID < $1.trackID }
        }
    }
}
