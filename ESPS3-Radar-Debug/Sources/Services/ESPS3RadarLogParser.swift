import Foundation

/// Compatibility adapter for existing integrations that consume single-source S3 events.
struct ESPS3RadarLogParser {
    private var parser = RadarLogParser()

    mutating func consume(_ bytes: [UInt8]) -> [S3RadarLogEvent] {
        parser.consume(bytes).compactMap { event in
            guard event.source == .s3Local else { return .ignored }
            switch event.kind {
            case .frameStarted:
                return .snapshotStarted
            case .target(let target, _):
                return .track(S3RadarTrack(trackID: target.trackID,
                                           xMillimeters: target.xMillimeters,
                                           yMillimeters: target.yMillimeters,
                                           distanceMillimeters: target.distanceMillimeters,
                                           angleDegrees: target.angleDegrees,
                                           speedCentimetersPerSecond: target.speedCentimetersPerSecond,
                                           confidence: target.confidence,
                                           isVisible: target.isVisible))
            case .update:
                return .ignored
            case .room, .home:
                return .ignored
            }
        }
    }
}
