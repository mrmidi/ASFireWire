import SwiftUI

struct StereoMetersView: View {
    let metrics: AudioObserverMetrics
    let active: Bool

    private func db(_ level: Float) -> String {
        level > 0.000001 ? String(format: "%.1f", 20 * log10(level)) : "−∞"
    }

    private func meter(_ title: String, rms: Float, peak: Float) -> some View {
        let level = active ? max(0, rms) : 0
        let maximum = active ? max(0, peak) : 0
        return VStack(spacing: 4) {
            Text(title).font(.caption.weight(.medium))
            Text(db(level)).font(.system(size: 9, design: .monospaced))
            GeometryReader { geometry in
                let height = geometry.size.height
                let fraction = CGFloat(max(0, min(1, (20 * log10(max(level, 0.000001)) + 60) / 66)))
                let marker = CGFloat(max(0, min(1, (20 * log10(max(maximum, 0.000001)) + 60) / 66)))
                ZStack {
                    RoundedRectangle(cornerRadius: 5).fill(.white.opacity(0.08))
                    VStack {
                        Spacer(minLength: 0)
                        RoundedRectangle(cornerRadius: 4).fill(.mint)
                            .frame(height: max(2, height * fraction))
                    }
                    Rectangle().fill(.orange)
                        .frame(height: 2)
                        .position(x: geometry.size.width / 2,
                                  y: height * (1 - marker))
                }
            }
            Text("−60").foregroundStyle(.secondary)
                .font(.system(size: 8, design: .monospaced))
        }
        .frame(width: 38)
    }

    private func scale(_ title: String, value: Float, left: String, right: String,
                       valid: Bool = true) -> some View {
        VStack(alignment: .leading, spacing: 3) {
            HStack {
                Text(title)
                Spacer(minLength: 4)
                Text(active && valid ? String(format: "%.2f", value) : "—")
            }
            GeometryReader { geometry in
                ZStack(alignment: .leading) {
                    Capsule().fill(.white.opacity(0.10)).frame(height: 4)
                    Rectangle().fill(.gray).frame(width: 1, height: 12)
                        .offset(x: geometry.size.width / 2)
                    if active && valid {
                        Circle().fill(value < 0 && title == "Correlation" ? .orange : .mint)
                            .frame(width: 8, height: 8)
                            .offset(x: max(0, (geometry.size.width - 8) * CGFloat((value + 1) / 2)))
                    }
                }
            }
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
                meter("L", rms: values[2], peak: metrics.leftPeak)
                meter("R", rms: values[3], peak: metrics.rightPeak)
                meter("M", rms: values[4], peak: values[0])
                meter("S", rms: values[5], peak: values[1])
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)

            VStack(spacing: 7) {
                scale("Correlation", value: metrics.correlation, left: "−1", right: "+1",
                      valid: metrics.correlationValid)
                HStack {
                    Text("Rolling (1 s)").foregroundStyle(.secondary)
                    Spacer(minLength: 4)
                    Text(active && metrics.correlationValid
                         ? String(format: "%+.2f", metrics.correlationAverage) : "—")
                }
                scale("Balance", value: values[6], left: "L", right: "R")
                scale("Stereo width", value: min(1, max(-1, 2 * values[7] - 1)),
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
