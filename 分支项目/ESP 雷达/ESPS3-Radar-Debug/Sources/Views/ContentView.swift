import SwiftUI
import UniformTypeIdentifiers

struct ContentView: View {
    @ObservedObject var store: RadarStore
    @State private var showsImporter = false

    var body: some View {
        DashboardView(store: store)
            .toolbar {
                ToolbarItemGroup(placement: .primaryAction) {
                    Button("导入日志", systemImage: "doc.badge.plus") { showsImporter = true }
                    Button("刷新串口", systemImage: "arrow.clockwise") { store.refreshDevices() }
                    Button(store.isParsing ? "停止解析" : "启动解析",
                           systemImage: store.isParsing ? "stop.fill" : "play.fill") {
                        store.startOrStopParsing()
                    }
                    .keyboardShortcut(.return, modifiers: [.command])
                    .disabled(!store.isParsing && store.selectedDevice.isEmpty)
                }
            }
            .fileImporter(isPresented: $showsImporter,
                          allowedContentTypes: [.plainText, .utf8PlainText, .text]) { result in
                if case .success(let url) = result { store.importReplayLog(from: url) }
            }
    }
}
