import SwiftUI

struct RadarCanvas: View {
    let source: RadarSource
    let targets: [RadarTarget]
    let trackHistory: [Int: [RadarTrackPoint]]
    let coordinateConfig: RadarCoordinateConfig
    let zoneConfig: RadarZoneConfig

    var body: some View {
        Canvas { context, size in
            let geometry = RadarCanvasGeometry(size: size, config: coordinateConfig, targets: targets)
            drawGrid(context: context, geometry: geometry)
            if zoneConfig.showsZones { drawZones(context: context, geometry: geometry) }
            drawTrails(context: context, geometry: geometry)
            for target in targets { drawTarget(target, context: context, geometry: geometry) }
        }
        .background(.thinMaterial, in: RoundedRectangle(cornerRadius: 8))
        .accessibilityLabel("\(source.displayName) 独立 LD2450 雷达图")
    }

    private func drawGrid(context: GraphicsContext, geometry: RadarCanvasGeometry) {
        let grid = Color.secondary.opacity(0.35)
        let range = max(1, Int(geometry.maximumRangeMeters.rounded(.up)))
        for meter in 1...range {
            let radius = geometry.radius * CGFloat(meter) / geometry.maximumRangeMeters
            var path = Path()
            path.addArc(center: geometry.origin, radius: radius, startAngle: .degrees(180), endAngle: .degrees(360), clockwise: false)
            context.stroke(path, with: .color(grid), lineWidth: meter == range ? 1.2 : 0.6)
            if meter == range || meter.isMultiple(of: 2) {
                context.draw(Text("\(meter)m").font(.caption2).foregroundColor(.secondary),
                             at: CGPoint(x: geometry.origin.x + 4, y: geometry.origin.y - radius + 10),
                             anchor: .leading)
            }
        }
        for degree in stride(from: -60, through: 60, by: 30) {
            let angle = Double(degree) * .pi / 180
            let point = CGPoint(x: geometry.origin.x + geometry.radius * CGFloat(sin(angle)),
                                y: geometry.origin.y - geometry.radius * CGFloat(cos(angle)))
            var line = Path()
            line.move(to: geometry.origin)
            line.addLine(to: point)
            context.stroke(line, with: .color(grid), lineWidth: 0.6)
        }
        context.fill(Path(ellipseIn: CGRect(x: geometry.origin.x - 5, y: geometry.origin.y - 5, width: 10, height: 10)),
                     with: .color(.orange))
    }

    private func drawZones(context: GraphicsContext, geometry: RadarCanvasGeometry) {
        for zone in zoneConfig.zones {
            let lower = geometry.position(x: zone.minimumXMillimeters, y: zone.minimumYMillimeters)
            let upper = geometry.position(x: zone.maximumXMillimeters, y: zone.maximumYMillimeters)
            let rect = CGRect(x: min(lower.x, upper.x), y: min(lower.y, upper.y),
                              width: abs(upper.x - lower.x), height: abs(upper.y - lower.y))
            context.stroke(Path(rect), with: .color(.purple.opacity(0.55)), lineWidth: 1)
        }
    }

    private func drawTrails(context: GraphicsContext, geometry: RadarCanvasGeometry) {
        for (trackID, points) in trackHistory {
            guard points.count > 1 else { continue }
            var path = Path()
            for (index, point) in points.enumerated() {
                let location = geometry.position(x: point.xMillimeters, y: point.yMillimeters)
                if index == 0 { path.move(to: location) } else { path.addLine(to: location) }
            }
            context.stroke(path, with: .color(color(for: trackID).opacity(0.35)), lineWidth: 1.5)
        }
    }

    private func drawTarget(_ target: RadarTarget, context: GraphicsContext, geometry: RadarCanvasGeometry) {
        let point = geometry.position(x: target.xMillimeters, y: target.yMillimeters)
        let tint = color(for: target.trackID).opacity(target.isVisible ? 1 : 0.35)
        context.fill(Path(ellipseIn: CGRect(x: point.x - 8, y: point.y - 8, width: 16, height: 16)), with: .color(tint))
        context.fill(Path(ellipseIn: CGRect(x: point.x - 3, y: point.y - 3, width: 6, height: 6)), with: .color(.white))
        let label = Text("T\(target.trackID)  \(String(format: "%.1fm", target.distanceMeters))")
            .font(.caption.weight(.semibold)).foregroundColor(.primary)
        context.draw(label, at: CGPoint(x: point.x + 12, y: point.y - 10), anchor: .leading)
    }

    private func color(for id: Int) -> Color {
        switch (id - 1) % 4 {
        case 0: .blue
        case 1: .orange
        case 2: .green
        default: .red
        }
    }
}

private struct RadarCanvasGeometry {
    let size: CGSize
    let config: RadarCoordinateConfig
    let origin: CGPoint
    let radius: CGFloat
    let maximumRangeMeters: CGFloat

    init(size: CGSize, config: RadarCoordinateConfig, targets: [RadarTarget]) {
        self.size = size
        self.config = config
        origin = CGPoint(x: size.width / 2, y: size.height - 34)
        radius = min(size.width * 0.44, size.height - 70)
        let xRange = max(abs(config.minimumXMillimeters - config.originXMillimeters), abs(config.maximumXMillimeters - config.originXMillimeters))
        let yRange = max(abs(config.minimumYMillimeters - config.originYMillimeters), abs(config.maximumYMillimeters - config.originYMillimeters))
        let targetRange = targets.reduce(6_000) { range, target in
            max(range, target.distanceMillimeters, abs(target.xMillimeters), abs(target.yMillimeters))
        }
        maximumRangeMeters = max(6, CGFloat(max(xRange, yRange, targetRange)) / 1_000)
    }

    func position(x: Int, y: Int) -> CGPoint {
        var horizontal = Double(x - config.originXMillimeters - config.radarMountXMillimeters)
        var vertical = Double(y - config.originYMillimeters - config.radarMountYMillimeters)
        if config.flipX { horizontal *= -1 }
        if config.flipY { vertical *= -1 }
        let radians = config.rotationDegrees * .pi / 180
        let rotatedX = horizontal * cos(radians) - vertical * sin(radians)
        let rotatedY = horizontal * sin(radians) + vertical * cos(radians)
        let scale = radius / (maximumRangeMeters * 1_000)
        return CGPoint(x: origin.x + CGFloat(rotatedX) * scale,
                       y: origin.y - CGFloat(rotatedY) * scale)
    }
}
