import SwiftUI

struct StereoMetersView: View {
    let client: ASFWAudioObserverClient
    let metrics: AudioObserverMetrics
    let active: Bool

    private func db(_ level: Float) -> String {
        level > 0.000001 ? String(format: "%.1f", 20 * log10(level)) : "−∞"
    }

    private func meter(_ title: String, index: UInt32, rms: Float) -> some View {
        let level = active ? max(0, rms) : 0
        return VStack(spacing: 4) {
            Text(title).font(.caption.weight(.medium))
            Text(db(level)).font(.system(size: 9, design: .monospaced))
            MetalAnalyzerPlotView(client: client, mode: 0, index: index)
                .background(.white.opacity(0.08))
                .clipShape(RoundedRectangle(cornerRadius: 5))
            Text("−60").foregroundStyle(.secondary)
                .font(.system(size: 8, design: .monospaced))
        }
        .frame(width: 38)
    }

    private func scale(_ title: String, index: UInt32, value: Float, left: String, right: String,
                       valid: Bool = true) -> some View {
        VStack(alignment: .leading, spacing: 3) {
            HStack {
                Text(title)
                Spacer(minLength: 4)
                Text(active && valid ? String(format: "%.2f", value) : "—")
            }
            MetalAnalyzerPlotView(client: client, mode: 1, index: index)
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
        let values = metrics.meterValues
        HStack(alignment: .center, spacing: 12) {
            HStack(alignment: .bottom, spacing: 4) {
                meter("L", index: 0, rms: values[2])
                meter("R", index: 1, rms: values[3])
                meter("M", index: 2, rms: values[4])
                meter("S", index: 3, rms: values[5])
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)

            VStack(spacing: 7) {
                scale("Correlation", index: 0, value: metrics.correlation, left: "−1", right: "+1",
                      valid: metrics.correlationValid)
                HStack {
                    Text("Rolling (1 s)").foregroundStyle(.secondary)
                    Spacer(minLength: 4)
                    Text(active && metrics.correlationValid
                         ? String(format: "%+.2f", metrics.correlationAverage) : "—")
                }
                scale("Balance", index: 1, value: values[6], left: "L", right: "R")
                scale("Stereo width", index: 2, value: min(1, max(-1, 2 * values[7] - 1)),
                      left: "0", right: "100%")
                HStack {
                    Text("Side energy").foregroundStyle(.secondary)
                    Spacer(minLength: 4)
                    Text(active ? "\(db(values[5])) dBFS" : "—")
                }
            }
            .frame(width: 168)
        }
        .font(.caption.monospacedDigit())
    }
}
