// swift-tools-version: 6.0
import PackageDescription

let package = Package(
    name: "ESPS3RadarDebug",
    platforms: [.macOS(.v14)],
    products: [
        .executable(name: "ESPS3RadarDebug", targets: ["ESPS3RadarDebug"]),
    ],
    targets: [
        .executableTarget(name: "ESPS3RadarDebug", path: "Sources"),
    ]
)
