import Foundation

struct RadarTarget: Identifiable, Equatable {
    let source: RadarSource
    let trackID: Int
    let xMillimeters: Int
    let yMillimeters: Int
    let rawXMillimeters: Int?
    let rawYMillimeters: Int?
    let filteredXMillimeters: Int?
    let filteredYMillimeters: Int?
    let distanceMillimeters: Int
    let angleDegrees: Int
    let speedCentimetersPerSecond: Int
    let directionDegrees: Int?
    let confidence: Int
    let isVisible: Bool
    let timestamp: Date
    let lastSeenTime: Date

    init(source: RadarSource = .unknown,
         trackID: Int,
         xMillimeters: Int,
         yMillimeters: Int,
         rawXMillimeters: Int? = nil,
         rawYMillimeters: Int? = nil,
         filteredXMillimeters: Int? = nil,
         filteredYMillimeters: Int? = nil,
         distanceMillimeters: Int,
         angleDegrees: Int,
         speedCentimetersPerSecond: Int,
         directionDegrees: Int? = nil,
         confidence: Int,
         isVisible: Bool,
         timestamp: Date,
         lastSeenTime: Date? = nil) {
        self.source = source
        self.trackID = trackID
        self.xMillimeters = xMillimeters
        self.yMillimeters = yMillimeters
        self.rawXMillimeters = rawXMillimeters
        self.rawYMillimeters = rawYMillimeters
        self.filteredXMillimeters = filteredXMillimeters
        self.filteredYMillimeters = filteredYMillimeters
        self.distanceMillimeters = distanceMillimeters
        self.angleDegrees = angleDegrees
        self.speedCentimetersPerSecond = speedCentimetersPerSecond
        self.directionDegrees = directionDegrees
        self.confidence = confidence
        self.isVisible = isVisible
        self.timestamp = timestamp
        self.lastSeenTime = lastSeenTime ?? timestamp
    }

    var id: Int { trackID }
    var distanceMeters: Double { Double(distanceMillimeters) / 1_000 }
    var targetAge: TimeInterval { max(0, Date().timeIntervalSince(lastSeenTime)) }
    var isStale: Bool { !isVisible }

    // Names matching the S3 local-log contract for parser-facing callers.
    var trackId: Int { trackID }
    var rawX: Int? { rawXMillimeters }
    var rawY: Int? { rawYMillimeters }
    var filteredX: Int? { filteredXMillimeters }
    var filteredY: Int? { filteredYMillimeters }
    var distance: Int { distanceMillimeters }
    var angle: Int { angleDegrees }
    var speed: Int { speedCentimetersPerSecond }
    var direction: Int? { directionDegrees }
    var visible: Bool { isVisible }
}

struct RadarSample: Identifiable, Equatable {
    let id = UUID()
    let source: RadarSource
    let timestamp: Date
    let targets: [RadarTarget]
}

struct RadarTrackPoint: Equatable {
    let xMillimeters: Int
    let yMillimeters: Int
    let timestampMs: Int64
}

struct RadarTrack: Equatable {
    let trackerID: Int
    var latestTarget: RadarTarget
    var lastSeenMs: Int64
}

enum RadarPresenceState: String, CaseIterable, Equatable {
    case unknown
    case occupied
    case vacant
    case motion
    case hold
    case vacantInferred = "vacant_inferred"
}

enum RadarFreshnessState: String, Equatable {
    case fresh
    case stale
    case offline
    case neverSeen = "never_seen"
}

struct RadarZone: Identifiable, Equatable, Codable {
    let id: UUID
    var name: String
    var minimumXMillimeters: Int
    var maximumXMillimeters: Int
    var minimumYMillimeters: Int
    var maximumYMillimeters: Int

    init(id: UUID = UUID(),
         name: String,
         minimumXMillimeters: Int,
         maximumXMillimeters: Int,
         minimumYMillimeters: Int,
         maximumYMillimeters: Int) {
        self.id = id
        self.name = name
        self.minimumXMillimeters = minimumXMillimeters
        self.maximumXMillimeters = maximumXMillimeters
        self.minimumYMillimeters = minimumYMillimeters
        self.maximumYMillimeters = maximumYMillimeters
    }
}

struct RadarCoordinateConfig: Equatable, Codable {
    var minimumXMillimeters: Int = -6_000
    var maximumXMillimeters: Int = 6_000
    var minimumYMillimeters: Int = 0
    var maximumYMillimeters: Int = 6_000
    var originXMillimeters: Int = 0
    var originYMillimeters: Int = 0
    var flipX = false
    var flipY = false
    var rotationDegrees = 0.0
    var radarMountXMillimeters = 0
    var radarMountYMillimeters = 0
    var roomBoundaryXMillimeters = 6_000
    var roomBoundaryYMillimeters = 6_000
    var trackRetentionMilliseconds: Int64 = 120_000
}

struct RadarZoneConfig: Equatable, Codable {
    var showsZones = true
    var zones: [RadarZone] = []
}

struct RadarRoomConfig: Equatable, Codable {
    var roomName: String
    var coordinateConfig: RadarCoordinateConfig
    var zoneConfig: RadarZoneConfig

    static func `default`(for source: RadarSource) -> RadarRoomConfig {
        RadarRoomConfig(roomName: source.defaultRoomName,
                        coordinateConfig: RadarCoordinateConfig(),
                        zoneConfig: RadarZoneConfig())
    }
}

struct RadarFreshnessPolicy: Equatable {
    static let `default` = RadarFreshnessPolicy(freshAfterMilliseconds: 3_000,
                                                 offlineAfterMilliseconds: 10_000)

    let freshAfterMilliseconds: Int64
    let offlineAfterMilliseconds: Int64
}

struct RadarUARTHealth: Equatable {
    var rxBytes = 0
    var timeout = 0
    var fifoOverflow = 0
}

struct RadarParserHealth: Equatable {
    var acceptedFrames = 0
    var badHeader = 0
    var badTail = 0
    var resyncCount = 0

    var parseErrors: Int { badHeader + badTail }
}

struct RadarStatePatch: Equatable {
    var online: Bool?
    var connectionType: String?
    var sequence: Int64?
    var targetCount: Int?
    var presenceState: RadarPresenceState?
    var motionState: String?
    var spatialState: String?
    var parserErrors: Int?
    var sequenceRejects: Int?
    var identityMismatches: Int?
    var droppedFrames: Int?
    var recoveryState: String?
    var deviceID: String?
    var sensorState: String?
    var occupancyState: String?
    var acceptedFrames: Int?
    var badHeader: Int?
    var badTail: Int?
    var resyncCount: Int?
    var rxBytes: Int?
    var timeout: Int?
    var fifoOverflow: Int?
    var updatesTargetPosition: Bool
    var isValidFrame: Bool

    init(online: Bool? = nil,
         connectionType: String? = nil,
         sequence: Int64? = nil,
         targetCount: Int? = nil,
         presenceState: RadarPresenceState? = nil,
         motionState: String? = nil,
         spatialState: String? = nil,
         parserErrors: Int? = nil,
         sequenceRejects: Int? = nil,
         identityMismatches: Int? = nil,
         droppedFrames: Int? = nil,
         recoveryState: String? = nil,
         deviceID: String? = nil,
         sensorState: String? = nil,
         occupancyState: String? = nil,
         acceptedFrames: Int? = nil,
         badHeader: Int? = nil,
         badTail: Int? = nil,
         resyncCount: Int? = nil,
         rxBytes: Int? = nil,
         timeout: Int? = nil,
         fifoOverflow: Int? = nil,
         updatesTargetPosition: Bool = false,
         isValidFrame: Bool = false) {
        self.online = online
        self.connectionType = connectionType
        self.sequence = sequence
        self.targetCount = targetCount
        self.presenceState = presenceState
        self.motionState = motionState
        self.spatialState = spatialState
        self.parserErrors = parserErrors
        self.sequenceRejects = sequenceRejects
        self.identityMismatches = identityMismatches
        self.droppedFrames = droppedFrames
        self.recoveryState = recoveryState
        self.deviceID = deviceID
        self.sensorState = sensorState
        self.occupancyState = occupancyState
        self.acceptedFrames = acceptedFrames
        self.badHeader = badHeader
        self.badTail = badTail
        self.resyncCount = resyncCount
        self.rxBytes = rxBytes
        self.timeout = timeout
        self.fifoOverflow = fifoOverflow
        self.updatesTargetPosition = updatesTargetPosition
        self.isValidFrame = isValidFrame
    }

    var hasValues: Bool {
        online != nil || connectionType != nil || sequence != nil || targetCount != nil ||
            presenceState != nil || motionState != nil || spatialState != nil ||
            parserErrors != nil || sequenceRejects != nil || identityMismatches != nil ||
            droppedFrames != nil || recoveryState != nil || deviceID != nil || sensorState != nil ||
            occupancyState != nil || acceptedFrames != nil || badHeader != nil || badTail != nil ||
            resyncCount != nil || rxBytes != nil || timeout != nil || fifoOverflow != nil
    }
}

enum RadarLogEventKind: Equatable {
    case frameStarted(RadarStatePatch)
    case target(RadarTarget, RadarStatePatch)
    case update(RadarStatePatch)
}

struct RadarLogEvent: Equatable {
    let source: RadarSource
    let sourceReason: String
    let rawLine: String
    let timestampMilliseconds: Int64?
    let kind: RadarLogEventKind
}

struct UnknownRadarDiagnostic: Identifiable, Equatable {
    let id = UUID()
    let rawSummary: String
    let reason: String
    let candidateSource: String?
    let candidateDeviceID: String?
    let timestampMilliseconds: Int64
}

struct RadarRoomState: Equatable {
    let source: RadarSource
    var sourceId: UInt8
    var deviceId: String
    var roomName: String
    var connectionType: String
    var online: Bool
    var lastUpdateMilliseconds: Int64?
    var lastSequence: Int64?
    var frameRate: Double
    var rawTargets: [RadarTarget]
    var filteredTargets: [RadarTarget]
    var tracks: [Int: RadarTrack]
    var trackHistory: [Int: [RadarTrackPoint]]
    var targetCount: Int
    var presenceState: RadarPresenceState
    var motionState: String
    var spatialState: String
    var coordinateConfig: RadarCoordinateConfig
    var zoneConfig: RadarZoneConfig
    var parseErrorCount: Int
    var sequenceRejectCount: Int
    var identityMismatchCount: Int
    var staleFrameCount: Int
    var droppedFrameCount: Int
    var freshnessState: RadarFreshnessState
    var trackerIDAllocator: Int
    var targetDisappearanceMilliseconds: [Int: Int64]
    var stableTargetCount: Int
    var recoveryState: String
    var sensorState: String
    var occupancyState: String
    var uartHealth: RadarUARTHealth
    var parserHealth: RadarParserHealth
    var lastParserHealth: RadarParserHealth
    var lastValidTargets: [RadarTarget]
    var lastValidTimestamp: Int64?

    init(source: RadarSource, config: RadarRoomConfig) {
        self.source = source
        sourceId = source.sourceId
        deviceId = source.defaultDeviceID
        roomName = config.roomName
        connectionType = source.defaultConnectionType
        online = false
        lastUpdateMilliseconds = nil
        lastSequence = nil
        frameRate = 0
        rawTargets = []
        filteredTargets = []
        tracks = [:]
        trackHistory = [:]
        targetCount = 0
        presenceState = .unknown
        motionState = "unknown"
        spatialState = "unknown"
        coordinateConfig = config.coordinateConfig
        zoneConfig = config.zoneConfig
        parseErrorCount = 0
        sequenceRejectCount = 0
        identityMismatchCount = 0
        staleFrameCount = 0
        droppedFrameCount = 0
        freshnessState = .neverSeen
        trackerIDAllocator = 1
        targetDisappearanceMilliseconds = [:]
        stableTargetCount = 0
        recoveryState = "unknown"
        sensorState = "unknown"
        occupancyState = "unknown"
        uartHealth = RadarUARTHealth()
        parserHealth = RadarParserHealth()
        lastParserHealth = RadarParserHealth()
        lastValidTargets = []
        lastValidTimestamp = nil
    }

    var dataAgeMilliseconds: Int64? {
        guard let lastUpdateMilliseconds else { return nil }
        return max(0, RadarClock.nowMilliseconds - lastUpdateMilliseconds)
    }

    var acceptedFrames: Int { parserHealth.acceptedFrames }
    var badHeader: Int { parserHealth.badHeader }
    var badTail: Int { parserHealth.badTail }
    var resyncCount: Int { parserHealth.resyncCount }
}

enum RadarClock {
    static var nowMilliseconds: Int64 { Int64(Date().timeIntervalSince1970 * 1_000) }
}

// Legacy parser-facing types remain available for single-source integrations.
struct S3RadarTrack: Equatable {
    let trackID: Int
    let xMillimeters: Int
    let yMillimeters: Int
    let distanceMillimeters: Int
    let angleDegrees: Int
    let speedCentimetersPerSecond: Int
    let confidence: Int
    let isVisible: Bool
}

enum S3RadarLogEvent: Equatable {
    case snapshotStarted
    case track(S3RadarTrack)
    case ignored
}
