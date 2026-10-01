import SwiftUI

struct StereoMetersView: View {
    let client: ASFWAudioObserverClient
    let active: Bool

    private func db(_ level: Float) -> String {
        level > 0.000001 ? String(format: "%.1f", 20 * log10(level)) : "−∞"
    }

    private func meter(_ title: String, index: UInt32) -> some View {
        return VStack(spacing: 4) {
            Text(title).font(.caption.weight(.medium))
            AnalyzerMetalText("meter.\(title)", style: .meterLabel, template: "-00.0",
                              alignment: .center) { metrics, snapshot in
                db(snapshot.ioRunning ? max(0, metrics.meterValues[Int(index) + 2]) : 0)
            }
            AnalyzerCanvasSlot(mode: 0, index: index)
                .background(.white.opacity(0.08))
                .clipShape(RoundedRectangle(cornerRadius: 5))
            Text("−60").foregroundStyle(.secondary)
                .font(.system(size: 8, design: .monospaced))
        }
        .frame(width: 38)
    }

    private func scale(_ title: String, index: UInt32, left: String, right: String) -> some View {
        VStack(alignment: .leading, spacing: 3) {
            HStack {
                Text(title)
                Spacer(minLength: 4)
                AnalyzerMetalText("scale.\(index)", style: .caption, template: "+0.00",
                                  alignment: .trailing) { metrics, snapshot in
                    let value = index == 0 ? metrics.correlation : (index == 1 ? metrics.meterValues[6] : 100 * metrics.meterValues[7])
                    let valid = index != 0 || metrics.correlationValid
                    return snapshot.ioRunning && valid ? String(format: index == 2 ? "%.0f%%" : "%.2f", value) : "—"
                }
            }
            AnalyzerCanvasSlot(mode: 1, index: index)
                .background { Capsule().fill(.white.opacity(0.10)).frame(height: 4) }
                .frame(height: 12)
            HStack {
                Text(left)
                Spacer(minLength: 4)
                Text(title == "Stereo width" ? "50" : "0")
                Spacer(minLength: 4)
                Text(right)
            }
            .foregroundStyle(.secondary)
        }
    }

    var body: some View {
        HStack(alignment: .center, spacing: 12) {
            HStack(alignment: .bottom, spacing: 4) {
                meter("L", index: 0)
                meter("R", index: 1)
                meter("M", index: 2)
                meter("S", index: 3)
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)

            VStack(spacing: 7) {
                scale("Correlation", index: 0, left: "−1", right: "+1")
                HStack {
                    Text("Rolling (1 s)").foregroundStyle(.secondary)
                    Spacer(minLength: 4)
                    AnalyzerMetalText("rolling", style: .caption, template: "+0.00",
                                      alignment: .trailing) { metrics, snapshot in
                        snapshot.ioRunning && metrics.correlationValid ? String(format: "%+.2f", metrics.correlationAverage) : "—"
                    }
                }
                scale("Balance", index: 1, left: "L", right: "R")
                scale("Stereo width", index: 2,
                      left: "0", right: "100%")
                HStack {
                    Text("Side energy").foregroundStyle(.secondary)
                    Spacer(minLength: 4)
                    AnalyzerMetalText("side", style: .caption, template: "-00.0 dBFS",
                                      alignment: .trailing) { metrics, snapshot in
                        snapshot.ioRunning ? "\(db(metrics.meterValues[5])) dBFS" : "—"
                    }
                }
            }
            .frame(width: 168)
        }
        .analyzerCanvas(client: client)
        .transformPreference(AnalyzerCanvasAnchors.self) { $0 = [:] }
        .font(.caption.monospacedDigit())
    }
}
