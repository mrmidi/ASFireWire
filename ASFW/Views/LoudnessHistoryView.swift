import SwiftUI

struct LoudnessHistoryView: View {
    let client: ASFWAudioObserverClient
    let hasHistory: Bool

    var body: some View {
        GeometryReader { geometry in
            let rect = CGRect(x: 36, y: 8, width: max(1, geometry.size.width - 46),
                              height: max(1, geometry.size.height - 30))
            ZStack {
                Color.black.opacity(0.35)
                MetalAnalyzerPlotView(client: client, mode: 3, index: 0,
                    regions: (0..<3).map { AnalyzerPlotRegion(mode: 3, index: UInt32($0), rect: rect) },
                    historyState: client.plotHistory)
                Canvas { context, size in
                    func label(_ text: String, _ point: CGPoint, _ anchor: UnitPoint = .center) {
                        context.draw(Text(text).font(.system(size: 9, design: .monospaced))
                            .foregroundStyle(.gray), at: point, anchor: anchor)
                    }
                    for db in [-6, -12, -18, -24, -30, -36] {
                        let y = rect.minY + CGFloat(-6 - db) / 30 * rect.height
                        var path = Path()
                        path.move(to: CGPoint(x: rect.minX, y: y))
                        path.addLine(to: CGPoint(x: rect.maxX, y: y))
                        context.stroke(path, with: .color(.white.opacity(0.10)), lineWidth: 0.5)
                        label("\(db)", CGPoint(x: rect.minX - 6, y: y), .trailing)
                    }
                    for second in stride(from: 0, through: 60, by: 10) {
                        let x = rect.minX + CGFloat(second) / 60 * rect.width
                        var path = Path()
                        path.move(to: CGPoint(x: x, y: rect.minY))
                        path.addLine(to: CGPoint(x: x, y: rect.maxY))
                        context.stroke(path, with: .color(.white.opacity(0.06)), lineWidth: 0.5)
                        label(second == 60 ? "now" : "−\(60 - second)s",
                              CGPoint(x: x, y: size.height - 9),
                              second == 0 ? .leading : second == 60 ? .trailing : .center)
                    }
                }.allowsHitTesting(false)
                if !hasHistory {
                    Text("History appears during playback").font(.caption).foregroundStyle(.secondary)
                }
            }
            .clipShape(RoundedRectangle(cornerRadius: 8))
        }
        .accessibilityLabel("60-second loudness history")
    }
}
