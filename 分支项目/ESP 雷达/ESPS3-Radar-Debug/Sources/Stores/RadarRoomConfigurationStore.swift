import Foundation

struct RadarRoomConfigurationStore {
    private let defaults: UserDefaults
    private let keyPrefix = "com.sensair.esps3radardebug.radar-room-config."

    init(defaults: UserDefaults = .standard) {
        self.defaults = defaults
    }

    func loadConfigs() -> [RadarSource: RadarRoomConfig] {
        Dictionary(uniqueKeysWithValues: RadarSource.allCases.compactMap { source in
            guard let data = defaults.data(forKey: key(for: source)),
                  let config = try? JSONDecoder().decode(RadarRoomConfig.self, from: data) else { return nil }
            return (source, config)
        })
    }

    func save(_ config: RadarRoomConfig, for source: RadarSource) {
        guard let data = try? JSONEncoder().encode(config) else { return }
        defaults.set(data, forKey: key(for: source))
    }

    private func key(for source: RadarSource) -> String {
        keyPrefix + String(source.rawValue)
    }
}
