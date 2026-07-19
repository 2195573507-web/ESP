import Combine
import Foundation

@MainActor
final class RadarStore: ObservableObject {
    @Published var devices: [String] = []
    @Published var selectedDevice = ""
    @Published var selectedBaudRate = 115_200
    @Published private(set) var isParsing = false
    @Published private(set) var radarStates: [RadarSource: RadarRoomState]
    @Published private(set) var unknownDiagnostics: [UnknownRadarDiagnostic]
    @Published private(set) var receivedBytes = 0
    @Published private(set) var completedRecords = 0
    @Published private(set) var lastChunkHex = "-"
    @Published private(set) var status = "未启动雷达日志解析"
    @Published var replay = RadarReplayController()
    @Published var showsDiagnostics = true

    private let serialPort = SerialPort()
    private let configurationStore = RadarRoomConfigurationStore()
    private var parser = RadarLogParser()
    private var stateStore: RadarStateStore

    init() {
        let configuredRooms = RadarRoomConfigurationStore().loadConfigs()
        stateStore = RadarStateStore(configs: configuredRooms)
        radarStates = stateStore.states
        unknownDiagnostics = stateStore.unknownDiagnostics
        refreshDevices()
        serialPort.onReceive = { [weak self] bytes in
            DispatchQueue.main.async { self?.consume(bytes) }
        }
        serialPort.onDisconnect = { [weak self] error in
            DispatchQueue.main.async {
                self?.isParsing = false
                self?.status = error.localizedDescription
            }
        }
    }

    var roomSources: [RadarSource] { RadarSource.roomSources }

    func state(for source: RadarSource) -> RadarRoomState {
        radarStates[source] ?? RadarRoomState(source: source, config: .default(for: source))
    }

    func refreshDevices() {
        devices = SerialPort.availableDevices()
        if selectedDevice.isEmpty || !devices.contains(selectedDevice) {
            selectedDevice = devices.first ?? ""
        }
        if !isParsing {
            status = devices.isEmpty ? "未发现 USB 串口设备；可导入日志回放" : "选择 ESPS3 串口后启动日志解析，或导入日志回放"
        }
    }

    func startOrStopParsing() {
        if isParsing {
            serialPort.close()
            isParsing = false
            status = "串口解析已停止"
            return
        }
        guard !selectedDevice.isEmpty else {
            status = "未选择 ESPS3 串口"
            return
        }
        do {
            try serialPort.open(path: selectedDevice, baudRate: selectedBaudRate)
            resetRuntimeState()
            parser = RadarLogParser()
            receivedBytes = 0
            completedRecords = 0
            lastChunkHex = "-"
            isParsing = true
            status = "正在解析 \(selectedDevice) 的三路雷达日志"
        } catch {
            status = error.localizedDescription
        }
    }

    func importReplayLog(from url: URL) {
        guard url.startAccessingSecurityScopedResource() else {
            status = "无法访问导入的日志文件"
            return
        }
        defer { url.stopAccessingSecurityScopedResource() }
        do {
            let text = try String(contentsOf: url, encoding: .utf8)
            replay.load(text)
            status = "已导入 \(replay.recordCount) 条日志记录；可开始回放"
        } catch {
            status = "导入日志失败：\(error.localizedDescription)"
        }
    }

    func playOrPauseReplay() {
        if replay.isPlaying {
            replay.pause()
            status = "回放已暂停"
        } else {
            replay.play { [weak self] line in self?.consumeReplayLine(line) }
            status = "正在回放三路交错日志"
        }
    }

    func restartReplay() {
        replay.restart { [weak self] in
            self?.resetRuntimeState()
            self?.parser = RadarLogParser()
        }
        status = "回放已重新开始；三路状态与 UNKNOWN 已复位"
    }

    func clearTrails(for source: RadarSource) {
        stateStore.clearTracks(for: source)
        publishState()
    }

    func updateConfig(_ config: RadarRoomConfig, for source: RadarSource) {
        stateStore.setConfig(config, for: source)
        configurationStore.save(config, for: source)
        publishState()
    }

    func resetRuntimeState() {
        stateStore.resetRuntimeState()
        publishState()
    }

    private func consume(_ bytes: [UInt8]) {
        receivedBytes += bytes.count
        lastChunkHex = bytes.suffix(24).map { String(format: "%02X", $0) }.joined(separator: " ")
        let events = parser.consume(bytes)
        consume(events)
    }

    private func consumeReplayLine(_ line: String) {
        let events = parser.consumeLine(line)
        consume(events)
    }

    private func consume(_ events: [RadarLogEvent]) {
        completedRecords += events.count
        for event in events { stateStore.apply(event) }
        publishState()
    }

    func refreshFreshness() {
        stateStore.refreshFreshness()
        publishState()
    }

    private func publishState() {
        radarStates = stateStore.states
        unknownDiagnostics = stateStore.unknownDiagnostics
    }
}
