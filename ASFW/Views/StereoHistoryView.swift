import SwiftUI

struct StereoHistoryView: View {
    @ObservedObject private var themeState = AnalyzerThemeState.shared

    private enum Series: String, CaseIterable, Identifiable {
        case correlation = "Correlation"
        case sideEnergy = "Side energy"

        var id: String { rawValue }
        var color: Color {
            self == .correlation
                ? Color(red: 0.10, green: 0.90, blue: 0.30)
                : Color(red: 0.10, green: 0.80, blue: 1.00)
        }
        var topLabel: String { self == .correlation ? "+1" : "100%" }
        var middleLabel: String { self == .correlation ? "0" : "50%" }
        var bottomLabel: String { self == .correlation ? "−1" : "0%" }
    }

    let client: ASFWAudioObserverClient
    let sampleRateHz: UInt32
    let active: Bool

    var body: some View {
        VStack(spacing: 8) {
            ForEach(Series.allCases) { series in
                VStack(alignment: .leading, spacing: 3) {
                    HStack(spacing: 6) {
                        Circle()
                            .fill(series.color)
                            .frame(width: 6, height: 6)
                        Text(series.rawValue)
                            .font(.caption.weight(.medium))
                            .foregroundStyle(themeState.mode.primaryTextColor)
                        Spacer()
                        AnalyzerMetalText("history.\(series.rawValue)", style: .caption, template: "100.0%",
                                          tone: .secondary, alignment: .trailing) { metrics, _ in
                            currentValue(for: series, metrics: metrics)
                        }
                    }
                    ZStack {
                        Color(red: 0.025, green: 0.035, blue: 0.05)

                        Canvas { context, size in draw(series, in: &context, size: size) }

                        AnalyzerCanvasSlot(mode: 2, index: series == .correlation ? 0 : 1)
                            .padding(.leading, 36).padding(.trailing, 8)
                            .padding(.top, 5).padding(.bottom, 16)
                    }
                    .frame(height: 74)
                    .clipShape(RoundedRectangle(cornerRadius: 6))
                    .overlay(RoundedRectangle(cornerRadius: 6).strokeBorder(Color.white.opacity(0.12), lineWidth: 1))
                }
            }
            Text(active ? "60-second history" : "History appears during playback")
                .font(.caption2)
                .foregroundStyle(themeState.mode.secondaryTextColor)
                .frame(maxWidth: .infinity, alignment: .trailing)
        }
    }

    private func currentValue(for series: Series, metrics: AudioObserverMetrics) -> String {
        guard metrics.correlationValid else { return "—" }
        switch series {
        case .correlation: return String(format: "%+.2f", metrics.correlationAverage)
        case .sideEnergy: return String(format: "%.1f%%", (metrics.analysis.stereo.sideEnergyFraction.value ?? 0) * 100)
        }
    }

    private func draw(_ series: Series, in context: inout GraphicsContext, size: CGSize) {
        let leftMargin: CGFloat = 36
        let rightMargin: CGFloat = 8
        let topMargin: CGFloat = 5
        let bottomMargin: CGFloat = 16
        let plot = CGRect(x: leftMargin, y: topMargin,
                          width: max(1, size.width - (leftMargin + rightMargin)),
                          height: max(1, size.height - (topMargin + bottomMargin)))
        let grid = Color.white.opacity(0.12)

        for fraction in [CGFloat(0), 0.5, 1] {
            let y = plot.minY + plot.height * fraction
            var line = Path()
            line.move(to: CGPoint(x: plot.minX, y: y))
            line.addLine(to: CGPoint(x: plot.maxX, y: y))
            let isZeroLine = series == .correlation && fraction == 0.5
            context.stroke(line, with: .color(isZeroLine ? Color.white.opacity(0.26) : grid),
                           lineWidth: isZeroLine ? 0.8 : 0.6)
        }

        for fraction in [CGFloat(0), 0.5, 1] {
            let x = plot.minX + plot.width * fraction
            var line = Path()
            line.move(to: CGPoint(x: x, y: plot.minY))
            line.addLine(to: CGPoint(x: x, y: plot.maxY))
            context.stroke(line, with: .color(grid.opacity(0.65)), lineWidth: 0.5)
        }

        func label(_ value: String, at point: CGPoint, anchor: UnitPoint = .center) {
            context.draw(Text(value).font(.system(size: 9, weight: .medium, design: .monospaced))
                .foregroundStyle(Color(red: 0.75, green: 0.82, blue: 0.90)), at: point, anchor: anchor)
        }

        label(series.topLabel, at: CGPoint(x: leftMargin - 5, y: plot.minY), anchor: .trailing)
        label(series.middleLabel, at: CGPoint(x: leftMargin - 5, y: plot.midY), anchor: .trailing)
        label(series.bottomLabel, at: CGPoint(x: leftMargin - 5, y: plot.maxY), anchor: .trailing)
        label("−60s", at: CGPoint(x: plot.minX, y: size.height - 7), anchor: .leading)
        label("−30s", at: CGPoint(x: plot.midX, y: size.height - 7))
        label("now", at: CGPoint(x: plot.maxX, y: size.height - 7), anchor: .trailing)
    }
}
