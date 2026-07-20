import SwiftUI

struct DashboardView: View {
    @ObservedObject var store: RadarStore
    @State private var selectedRoom: RadarSource = .s3Local

    private var visibleSources: [RadarSource] {
        RadarDashboardLayout.panels(visibleSource: store.replay.visibleSource)
    }

    var body: some View {
        VStack(spacing: 0) {
            connectionBar
            overview
            GeometryReader { proxy in
                if proxy.size.width >= 1_260, store.replay.visibleSource == nil {
                    ScrollView {
                        HStack(alignment: .top, spacing: 14) {
                            ForEach(store.roomSources) { source in panel(for: source) }
                        }
                        .padding(16)
                        diagnostics
                            .padding(.horizontal, 16)
                            .padding(.bottom, 16)
                    }
                } else {
                    VStack(spacing: 0) {
                        Picker("房间", selection: $selectedRoom) {
                            ForEach(visibleSources) { source in Text(source.displayName).tag(source) }
                        }
                        .pickerStyle(.segmented)
                        .padding(16)
                        ScrollView { panel(for: visibleSources.contains(selectedRoom) ? selectedRoom : visibleSources.first ?? .s3Local) }
                        diagnostics
                            .padding(.horizontal, 16)
                            .padding(.bottom, 16)
                    }
                }
            }
        }
        .navigationTitle("三房间雷达调试")
        .onReceive(Timer.publish(every: 1, on: .main, in: .common).autoconnect()) { _ in
            store.refreshFreshness()
        }
    }

    private func panel(for source: RadarSource) -> some View {
        RadarPanelView(source: source,
                       state: store.state(for: source),
                       onClearTrails: { store.clearTrails(for: source) },
                       onConfigChanged: { store.updateConfig($0, for: source) })
        .frame(minWidth: 350, maxWidth: .infinity, alignment: .top)
    }

    private var overview: some View {
        HStack(spacing: 10) {
            VStack(alignment: .leading, spacing: 2) {
                Text("业务人数总计").font(.caption).foregroundStyle(.secondary)
                Text("\(store.roomSources.reduce(0) { $0 + store.state(for: $1).businessPersonCount })")
                    .font(.title2.monospacedDigit())
                Text("各来源独立；UNKNOWN 时数字不作确定人数解释").font(.caption2).foregroundStyle(.secondary)
            }
            .frame(minWidth: 230, alignment: .leading)
            ForEach(store.roomSources) { source in
                let state = store.state(for: source)
                VStack(alignment: .leading, spacing: 2) {
                    Text(source.displayName).font(.caption).lineLimit(1)
                    Text("\(state.businessPersonCount) 人 · \(state.countState)").font(.caption.monospacedDigit())
                    Text(ageText(state)).font(.caption2).foregroundStyle(freshnessColor(state))
                }
                .frame(maxWidth: .infinity, alignment: .leading)
            }
        }
        .padding(.horizontal, 20)
        .padding(.vertical, 9)
        .background(.quaternary)
    }

    @ViewBuilder private var diagnostics: some View {
        if store.showsDiagnostics {
            UnknownDiagnosticsView(diagnostics: store.unknownDiagnostics)
        }
    }

    private var connectionBar: some View {
        HStack(spacing: 10) {
            Image(systemName: store.isParsing ? "checkmark.circle.fill" : "circle")
                .foregroundStyle(store.isParsing ? .green : .secondary)
            Text(store.status).lineLimit(1)
            Spacer()
            Picker("串口", selection: $store.selectedDevice) {
                if store.devices.isEmpty { Text("未发现设备").tag("") }
                ForEach(store.devices, id: \.self) { Text($0).tag($0) }
            }
            .labelsHidden().frame(width: 210).disabled(store.isParsing)
            Picker("波特率", selection: $store.selectedBaudRate) {
                ForEach([115_200, 230_400, 460_800, 921_600], id: \.self) { Text("\($0)").tag($0) }
            }
            .labelsHidden().frame(width: 105).disabled(store.isParsing)
            Button(store.isParsing ? "停止串口" : "启动串口", systemImage: store.isParsing ? "stop.fill" : "play.fill") {
                store.startOrStopParsing()
            }
            .buttonStyle(.borderedProminent).disabled(!store.isParsing && store.selectedDevice.isEmpty)
            if store.replay.isLoaded {
                Button(store.replay.isPlaying ? "暂停回放" : "继续回放", systemImage: store.replay.isPlaying ? "pause.fill" : "play.fill") {
                    store.playOrPauseReplay()
                }
                Button("重新开始", systemImage: "backward.end.fill") { store.restartReplay() }
                Picker("倍速", selection: $store.replay.speed) {
                    ForEach([0.5, 1, 2, 4], id: \.self) { Text("\($0, specifier: "%g")x").tag($0) }
                }
                .frame(width: 78)
                Picker("显示来源", selection: $store.replay.visibleSource) {
                    Text("全部三路").tag(RadarSource?.none)
                    ForEach(store.roomSources) { source in Text(source.displayName).tag(Optional(source)) }
                }
                .frame(width: 118)
            }
        }
        .padding(.horizontal, 20).padding(.vertical, 10).background(.bar)
    }

    private func ageText(_ state: RadarRoomState) -> String {
        guard let last = state.lastValidTimestamp else { return "never_seen" }
        return "\(state.freshnessState.rawValue) · \(max(0, RadarClock.nowMilliseconds - last)) ms"
    }

    private func freshnessColor(_ state: RadarRoomState) -> Color {
        switch state.freshnessState {
        case .fresh: .green
        case .stale: .orange
        case .offline: .red
        case .neverSeen: .secondary
        }
    }
}

private struct RadarPanelView: View {
    let source: RadarSource
    let state: RadarRoomState
    let onClearTrails: () -> Void
    let onConfigChanged: (RadarRoomConfig) -> Void
    @State private var config: RadarRoomConfig

    init(source: RadarSource,
         state: RadarRoomState,
         onClearTrails: @escaping () -> Void,
         onConfigChanged: @escaping (RadarRoomConfig) -> Void) {
        self.source = source
        self.state = state
        self.onClearTrails = onClearTrails
        self.onConfigChanged = onConfigChanged
        _config = State(initialValue: RadarRoomConfig(roomName: state.roomName,
                                                       coordinateConfig: state.coordinateConfig,
                                                       zoneConfig: state.zoneConfig))
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack(alignment: .top) {
                VStack(alignment: .leading, spacing: 2) {
                    Text(source.displayName).font(.headline)
                    Text(config.roomName).font(.subheadline).foregroundStyle(.secondary)
                    Text(state.connectionType).font(.caption).foregroundStyle(.secondary)
                    Text("source \(state.sourceId) · \(state.deviceId)")
                        .font(.caption2.monospaced())
                        .foregroundStyle(.secondary)
                }
                Spacer()
                VStack(alignment: .trailing, spacing: 2) {
                    Text(state.freshnessState.rawValue).foregroundStyle(statusColor).font(.caption.weight(.semibold))
                    Text(lastUpdateText).font(.caption2).foregroundStyle(.secondary)
                }
            }
            RadarCanvas(source: source,
                        targets: state.visibleTracks,
                        trackHistory: state.trackHistory,
                        coordinateConfig: config.coordinateConfig,
                        zoneConfig: config.zoneConfig)
                .frame(minHeight: 300)
            HStack {
                LabeledContent("Raw targets", value: "\(state.rawTargetCount)")
                Spacer()
                LabeledContent("Visible tracks", value: "\(state.visibleTrackCount)")
                Spacer()
                LabeledContent("Retained persons", value: "\(state.retainedPersonCount)")
            }
            HStack {
                LabeledContent("Business persons", value: "\(state.businessPersonCount)")
                Spacer()
                LabeledContent("Count state", value: state.countState)
                Spacer()
                LabeledContent("History tracks", value: "\(state.historyTargetCount)")
            }
            .font(.caption.monospacedDigit())
            targetTable
            Grid(horizontalSpacing: 8, verticalSpacing: 5) {
                GridRow { metric("Occupancy", state.occupancyState); metric("Motion", state.motionState) }
                GridRow { metric("解析错误", state.parseErrorCount); metric("序列拒绝", state.sequenceRejectCount) }
                GridRow { metric("身份不符", state.identityMismatchCount); metric("丢帧", state.droppedFrameCount) }
                GridRow { metric("Stale", state.staleFrameCount); metric("帧率", String(format: "%.1f Hz", state.frameRate)) }
                GridRow { metric("Parser accepted", state.acceptedFrames); metric("bad header", state.badHeader) }
                GridRow { metric("bad tail", state.badTail); metric("resync", state.resyncCount) }
                GridRow { metric("RX bytes", state.uartHealth.rxBytes); metric("timeout", state.uartHealth.timeout) }
                GridRow { metric("FIFO overflow", state.uartHealth.fifoOverflow); metric("恢复", state.recoveryState) }
            }
            HStack {
                Button("清空该来源轨迹", systemImage: "trash") { onClearTrails() }
                Spacer()
                Text("seq: \(state.lastSequence.map(String.init) ?? "-")").font(.caption.monospacedDigit()).foregroundStyle(.secondary)
            }
            RadarRoomConfigEditor(config: $config)
                .onChange(of: config) { _, value in onConfigChanged(value) }
        }
        .padding(14)
        .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 8))
    }

    private var targetTable: some View {
        VStack(alignment: .leading, spacing: 4) {
            Text("独立目标详情").font(.caption.weight(.semibold))
            if state.visibleTracks.isEmpty {
                Text("当前没有可见 confirmed track；历史路径不计入当前人员").font(.caption).foregroundStyle(.secondary)
            } else {
                ForEach(state.visibleTracks) { target in
                    HStack(spacing: 8) {
                        Text("T\(target.trackID)").font(.caption.monospacedDigit()).frame(width: 32, alignment: .leading)
                        Text(String(format: "X=%.1fm  Y=%.1fm", Double(target.xMillimeters) / 1_000, Double(target.yMillimeters) / 1_000))
                            .font(.caption.monospacedDigit())
                        Spacer()
                        Text("conf \(target.confidence)\(target.isStale ? " · stale" : "")").font(.caption.monospacedDigit())
                    }
                }
            }
        }
    }

    private func metric<Value: CustomStringConvertible>(_ title: String, _ value: Value) -> some View {
        VStack(alignment: .leading, spacing: 1) {
            Text(title).font(.caption2).foregroundStyle(.secondary)
            Text(value.description).font(.caption.monospacedDigit())
        }
        .frame(maxWidth: .infinity, alignment: .leading)
    }

    private var lastUpdateText: String {
        guard let last = state.lastValidTimestamp else { return "未收到有效帧" }
        return "\(max(0, RadarClock.nowMilliseconds - last)) ms"
    }

    private var statusColor: Color {
        switch state.freshnessState {
        case .fresh: .green
        case .stale: .orange
        case .offline: .red
        case .neverSeen: .secondary
        }
    }
}

private struct RadarRoomConfigEditor: View {
    @Binding var config: RadarRoomConfig

    var body: some View {
        DisclosureGroup("该房间坐标与 Zone 配置") {
            Grid(horizontalSpacing: 8, verticalSpacing: 6) {
                GridRow { TextField("房间名称", text: $config.roomName); TextField("旋转", value: $config.coordinateConfig.rotationDegrees, format: .number).frame(width: 90) }
                GridRow { TextField("X 最小", value: $config.coordinateConfig.minimumXMillimeters, format: .number); TextField("X 最大", value: $config.coordinateConfig.maximumXMillimeters, format: .number) }
                GridRow { TextField("Y 最小", value: $config.coordinateConfig.minimumYMillimeters, format: .number); TextField("Y 最大", value: $config.coordinateConfig.maximumYMillimeters, format: .number) }
                GridRow { TextField("原点 X", value: $config.coordinateConfig.originXMillimeters, format: .number); TextField("原点 Y", value: $config.coordinateConfig.originYMillimeters, format: .number) }
                GridRow { TextField("安装 X", value: $config.coordinateConfig.radarMountXMillimeters, format: .number); TextField("安装 Y", value: $config.coordinateConfig.radarMountYMillimeters, format: .number) }
                GridRow { TextField("房间宽", value: $config.coordinateConfig.roomBoundaryXMillimeters, format: .number); TextField("房间深", value: $config.coordinateConfig.roomBoundaryYMillimeters, format: .number) }
                GridRow { TextField("轨迹保留 ms", value: $config.coordinateConfig.trackRetentionMilliseconds, format: .number); Toggle("翻转 X", isOn: $config.coordinateConfig.flipX) }
                GridRow { Toggle("翻转 Y", isOn: $config.coordinateConfig.flipY); Toggle("显示 Zone", isOn: $config.zoneConfig.showsZones) }
            }
            .font(.caption)
            .textFieldStyle(.roundedBorder)
        }
        .font(.caption)
    }
}

private struct UnknownDiagnosticsView: View {
    let diagnostics: [UnknownRadarDiagnostic]

    var body: some View {
        DisclosureGroup("UNKNOWN 诊断区 (\(diagnostics.count))") {
            if diagnostics.isEmpty {
                Text("没有无法识别来源的日志。UNKNOWN 永不进入任意房间状态。").font(.caption).foregroundStyle(.secondary)
            } else {
                ForEach(diagnostics.suffix(8).reversed()) { item in
                    VStack(alignment: .leading, spacing: 2) {
                        Text(item.reason).font(.caption.weight(.semibold))
                        Text("source: \(item.candidateSource ?? "-") · device_id: \(item.candidateDeviceID ?? "-") · \(item.timestampMilliseconds)").font(.caption2.monospacedDigit())
                        Text(item.rawSummary).font(.caption2.monospaced()).lineLimit(2).textSelection(.enabled)
                    }
                    Divider()
                }
            }
        }
        .padding(12)
        .background(.yellow.opacity(0.1), in: RoundedRectangle(cornerRadius: 8))
    }
}
