import SwiftUI

struct StereoHistoryView: View {
    private enum Series: String, CaseIterable, Identifiable {
        case correlation = "Correlation"
        case sideEnergy = "Side energy"

        var id: String { rawValue }
        var color: Color { self == .correlation ? .green : .cyan }
        var range: ClosedRange<Float> { self == .correlation ? -1...1 : 0...1 }
        var topLabel: String { self == .correlation ? "+1" : "100%" }
        var middleLabel: String { self == .correlation ? "0" : "50%" }
        var bottomLabel: String { self == .correlation ? "−1" : "0%" }
    }

    let client: ASFWAudioObserverClient
    let points: [AudioStereoHistoryPoint]
    let sampleRateHz: UInt32
    let active: Bool

    var body: some View {
        VStack(spacing: 8) {
            ForEach(Series.allCases) { series in
                VStack(alignment: .leading, spacing: 3) {
                    HStack {
                        Text(series.rawValue).font(.caption.weight(.medium))
                        Spacer()
                        Text(currentValue(for: series))
                            .font(.caption.monospacedDigit()).foregroundStyle(.secondary)
                    }
                    ZStack {
                        Canvas { context, size in draw(series, in: &context, size: size) }
                        MetalAnalyzerPlotView(client: client, mode: 2,
                                              index: series == .correlation ? 0 : 1, points: points)
                            .padding(.leading, 34).padding(.trailing, 6)
                            .padding(.top, 5).padding(.bottom, 16)
                    }
                    .frame(height: 74)
                    .background(Color.black.opacity(0.22))
                    .clipShape(RoundedRectangle(cornerRadius: 6))
                }
            }
            Text(active ? "60-second history" : "History appears during playback")
                .font(.caption2).foregroundStyle(.tertiary)
                .frame(maxWidth: .infinity, alignment: .trailing)
        }
    }

    private func currentValue(for series: Series) -> String {
        guard let point = points.last else { return "—" }
        switch series {
        case .correlation: return String(format: "%+.2f", point.correlation)
        case .sideEnergy: return String(format: "%.1f%%", point.sideEnergyFraction * 100)
        }
    }

    private func draw(_ series: Series, in context: inout GraphicsContext, size: CGSize) {
        let plot = CGRect(x: 34, y: 5, width: max(1, size.width - 40), height: max(1, size.height - 21))
        let grid = Color.white.opacity(0.14)
        for fraction in [CGFloat(0), 0.5, 1] {
            let y = plot.minY + plot.height * fraction
            var line = Path()
            line.move(to: CGPoint(x: plot.minX, y: y))
            line.addLine(to: CGPoint(x: plot.maxX, y: y))
            context.stroke(line, with: .color(grid), lineWidth: 0.6)
        }
        for fraction in [CGFloat(0), 0.5, 1] {
            let x = plot.minX + plot.width * fraction
            var line = Path()
            line.move(to: CGPoint(x: x, y: plot.minY))
            line.addLine(to: CGPoint(x: x, y: plot.maxY))
            context.stroke(line, with: .color(grid.opacity(0.65)), lineWidth: 0.5)
        }

        func label(_ value: String, at point: CGPoint, anchor: UnitPoint = .center) {
            context.draw(Text(value).font(.system(size: 8, design: .monospaced))
                .foregroundStyle(.secondary), at: point, anchor: anchor)
        }
        label(series.topLabel, at: CGPoint(x: 29, y: plot.minY), anchor: .trailing)
        label(series.middleLabel, at: CGPoint(x: 29, y: plot.midY), anchor: .trailing)
        label(series.bottomLabel, at: CGPoint(x: 29, y: plot.maxY), anchor: .trailing)
        label("−60s", at: CGPoint(x: plot.minX, y: size.height - 7), anchor: .leading)
        label("−30s", at: CGPoint(x: plot.midX, y: size.height - 7))
        label("now", at: CGPoint(x: plot.maxX, y: size.height - 7), anchor: .trailing)

    }
}
