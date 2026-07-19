import Foundation

enum RadarSource: UInt8, CaseIterable, Identifiable, Codable, Hashable {
    case s3Local = 0
    case c51 = 1
    case c52 = 2
    case unknown = 255

    var id: UInt8 { rawValue }
    var sourceId: UInt8 { rawValue }

    var defaultDeviceID: String {
        switch self {
        case .s3Local: "sensair_s3_gateway_01"
        case .c51: "sensair_shuttle_01"
        case .c52: "sensair_shuttle_02"
        case .unknown: ""
        }
    }

    static let roomSources: [RadarSource] = [.s3Local, .c51, .c52]

    var displayName: String {
        switch self {
        case .s3Local: "S3 本地雷达"
        case .c51: "C51 房间雷达"
        case .c52: "C52 房间雷达"
        case .unknown: "UNKNOWN 诊断"
        }
    }

    var defaultRoomName: String {
        switch self {
        case .s3Local: "S3 本地房间"
        case .c51: "C51 房间"
        case .c52: "C52 房间"
        case .unknown: "未识别来源"
        }
    }

    var defaultConnectionType: String {
        switch self {
        case .s3Local: "UART / 本地雷达"
        case .c51: "LD2450 BLE -> C51 -> S3"
        case .c52: "LD2450 BLE -> C52 -> S3"
        case .unknown: "未知链路"
        }
    }

    static func identify(in text: String) -> (source: RadarSource, reason: String, candidate: String?, deviceID: String?) {
        let explicitSource = fieldValue(in: text, keys: ["source"])
        if let explicitSource {
            if let source = sourceValue(explicitSource) {
                return (source, "explicit source", explicitSource, fieldValue(in: text, keys: ["device_id", "device", "did"]))
            }
            return (.unknown, "unrecognized explicit source", explicitSource, fieldValue(in: text, keys: ["device_id", "device", "did"]))
        }

        let localSource = fieldValue(in: text, keys: ["local_source", "source_id"])
        if let localSource {
            if let source = sourceValue(localSource) {
                return (source, "local_source/source_id", localSource, fieldValue(in: text, keys: ["device_id", "local_id", "device", "did"]))
            }
            return (.unknown, "unrecognized local_source/source_id", localSource, fieldValue(in: text, keys: ["device_id", "local_id", "device", "did"]))
        }

        let deviceID = fieldValue(in: text, keys: ["device_id", "local_id", "device", "did"])
        if let deviceID {
            if let source = deviceSource(deviceID) {
                return (source, "device_id/local_id", nil, deviceID)
            }
            return (.unknown, "unrecognized device_id/local_id", nil, deviceID)
        }

        let uppercased = text.uppercased()
        if uppercased.contains("S3_LOCAL") || uppercased.contains("RADAR_LOCAL") ||
            text.range(of: #"\blocal\s+(?:sensor=|track=|accepted\s+index=|raw\s+slot=)"#, options: [.regularExpression, .caseInsensitive]) != nil {
            return (.s3Local, "log tag/fixed local field", nil, nil)
        }
        if uppercased.contains("ESPC51") || uppercased.contains("C51") {
            return (.c51, "log tag", nil, nil)
        }
        if uppercased.contains("ESPC52") || uppercased.contains("C52") {
            return (.c52, "log tag", nil, nil)
        }
        return (.unknown, "no supported source, local source, device id, or tag", nil, nil)
    }

    private static func sourceValue(_ value: String) -> RadarSource? {
        switch value.trimmingCharacters(in: .whitespacesAndNewlines).uppercased() {
        case "S3_LOCAL", "S3", "0": .s3Local
        case "C51", "ESPC51", "1": .c51
        case "C52", "ESPC52", "2": .c52
        default: nil
        }
    }

    private static func deviceSource(_ value: String) -> RadarSource? {
        switch value.trimmingCharacters(in: .whitespacesAndNewlines).lowercased() {
        case "sensair_s3_gateway_01": .s3Local
        case "sensair_shuttle_01": .c51
        case "sensair_shuttle_02": .c52
        default: nil
        }
    }

    static func fieldValue(in text: String, keys: [String]) -> String? {
        for key in keys {
            let escapedKey = NSRegularExpression.escapedPattern(for: key)
            let pattern = "(?:^|[\\s,\\[{])\(escapedKey)\\s*[=:]\\s*[\\\"']?([^,\\s}\\]\\\"]+)"
            guard let expression = try? NSRegularExpression(pattern: pattern, options: [.caseInsensitive]) else { continue }
            let range = NSRange(text.startIndex..., in: text)
            guard let match = expression.firstMatch(in: text, range: range),
                  let valueRange = Range(match.range(at: 1), in: text) else { continue }
            return String(text[valueRange])
        }
        return nil
    }
}
