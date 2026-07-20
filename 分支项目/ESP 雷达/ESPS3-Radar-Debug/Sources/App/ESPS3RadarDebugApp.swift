import SwiftUI

@main
struct ESPS3RadarDebugApp: App {
    @StateObject private var store = RadarStore()

    var body: some Scene {
        WindowGroup("ESPS3 Radar Debug") {
            ContentView(store: store)
                .frame(minWidth: 1_280, minHeight: 760)
        }
        .commands {
            CommandMenu("解析") {
                Button(store.isParsing ? "停止本地解析" : "启动本地解析") {
                    store.startOrStopParsing()
                }
                .keyboardShortcut(.return, modifiers: [.command])
                .disabled(!store.isParsing && store.selectedDevice.isEmpty)

                Button("清空 S3 轨迹") { store.clearTrails(for: .s3Local) }
                    .keyboardShortcut(.delete, modifiers: [.command])
            }
        }
    }
}
