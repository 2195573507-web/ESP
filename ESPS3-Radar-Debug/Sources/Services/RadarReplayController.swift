import Combine
import Foundation

struct RadarReplayRecord: Equatable, Identifiable {
    let id: Int
    let timestampMilliseconds: Int64
    let line: String
}

@MainActor
final class RadarReplayController: ObservableObject {
    @Published private(set) var isLoaded = false
    @Published private(set) var isPlaying = false
    @Published private(set) var currentIndex = 0
    @Published var speed = 1.0
    @Published var visibleSource: RadarSource?
    @Published private(set) var recordCount = 0

    private var records: [RadarReplayRecord] = []
    private var replayTask: Task<Void, Never>?
    private var process: ((String) -> Void)?
    private var playStartedAt: Date?
    private var replayStartedAtMilliseconds: Int64 = 0

    func load(_ text: String) {
        replayTask?.cancel()
        records = text
            .split(whereSeparator: \.isNewline)
            .enumerated()
            .map { index, line in
                let content = String(line)
                return RadarReplayRecord(id: index,
                                         timestampMilliseconds: replayTimestamp(in: content, fallback: Int64(index) * 100),
                                         line: content)
            }
            .sorted { lhs, rhs in
                lhs.timestampMilliseconds == rhs.timestampMilliseconds ? lhs.id < rhs.id : lhs.timestampMilliseconds < rhs.timestampMilliseconds
            }
        isLoaded = !records.isEmpty
        isPlaying = false
        currentIndex = 0
        recordCount = records.count
        replayStartedAtMilliseconds = records.first?.timestampMilliseconds ?? 0
    }

    func play(process: @escaping (String) -> Void) {
        guard isLoaded, currentIndex < records.count else { return }
        isPlaying = true
        playStartedAt = Date().addingTimeInterval(-elapsedReplaySeconds)
        self.process = process
        replayTask?.cancel()
        replayTask = Task { @MainActor [weak self] in
            while let self, self.isPlaying, !Task.isCancelled {
                self.advance()
                try? await Task.sleep(for: .milliseconds(20))
            }
        }
    }

    func pause() {
        replayTask?.cancel()
        replayTask = nil
        isPlaying = false
    }

    func restart(reset: () -> Void) {
        pause()
        currentIndex = 0
        replayStartedAtMilliseconds = records.first?.timestampMilliseconds ?? 0
        reset()
    }

    @discardableResult
    func step(process: (String) -> Void) -> Bool {
        guard currentIndex < records.count else {
            pause()
            return false
        }
        process(records[currentIndex].line)
        currentIndex += 1
        if currentIndex == records.count { pause() }
        return true
    }

    private func advance() {
        guard isPlaying else { return }
        let elapsedMilliseconds = Int64(elapsedReplaySeconds * 1_000 * speed)
        let targetTimestamp = replayStartedAtMilliseconds + elapsedMilliseconds
        while currentIndex < records.count, records[currentIndex].timestampMilliseconds <= targetTimestamp {
            process?(records[currentIndex].line)
            currentIndex += 1
        }
        if currentIndex == records.count { pause() }
    }

    private var elapsedReplaySeconds: TimeInterval {
        guard let playStartedAt else { return 0 }
        return max(0, Date().timeIntervalSince(playStartedAt))
    }

    private func replayTimestamp(in line: String, fallback: Int64) -> Int64 {
        if let value = RadarSource.fieldValue(in: line, keys: ["timestamp_ms", "ts_ms", "time_ms", "timestamp"]),
           let timestamp = Int64(value) {
            return timestamp
        }
        let pattern = #"\((\d{1,13})\)"#
        guard let expression = try? NSRegularExpression(pattern: pattern),
              let match = expression.firstMatch(in: line, range: NSRange(line.startIndex..., in: line)),
              let range = Range(match.range(at: 1), in: line),
              let timestamp = Int64(line[range]) else { return fallback }
        return timestamp
    }
}
