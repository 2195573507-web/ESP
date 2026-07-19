import Foundation

enum RadarDashboardLayout {
    static func panels(visibleSource: RadarSource?) -> [RadarSource] {
        if let visibleSource { return [visibleSource] }
        return RadarSource.roomSources
    }
}
