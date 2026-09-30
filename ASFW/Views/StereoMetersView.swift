import SwiftUI

struct StereoMetersView: View {
    let metrics: AudioObserverMetrics
    let active: Bool

    private func db(_ level: Float) -> String {
        level > 0.000001 ? String(format: "%.1f", 20 * log10(level)) : "−∞"
    }

    private func meter(_ title: String, rms: Float, peak: Float) -> some View {
        let level = active ? rms : 0
        let maximum = active ? peak : 0
        return HStack {
            Text(title).frame(width: 18, alignment: .leading)
            GeometryReader { geometry in
                let width = geometry.size.width
                let fraction = CGFloat(max(0, min(1, (20 * log10(max(level, 0.000001)) + 60) / 66)))
                let marker = CGFloat(max(0, min(1, (20 * log10(max(maximum, 0.000001)) + 60) / 66)))
                ZStack(alignment: .leading) {
                    Capsule().fill(.white.opacity(0.08))
                    Capsule().fill(.mint).frame(width: width * fraction)
                    Rectangle().fill(.orange).frame(width: 2).offset(x: max(0, width * marker - 2))
                }
            }.frame(height: 8)
            Text("\(db(level)) / \(db(maximum))").frame(width: 104, alignment: .trailing)
        }
    }

    private func scale(_ title: String, value: Float, left: String, right: String, valid: Bool = true) -> some View {
        VStack(alignment: .leading, spacing: 4) {
            HStack { Text(title); Spacer(); Text(active && valid ? String(format: "%.2f", value) : "—") }
            GeometryReader { geometry in
                ZStack(alignment: .leading) {
                    Capsule().fill(.white.opacity(0.10)).frame(height: 4)
                    Rectangle().fill(.gray).frame(width: 1, height: 12).offset(x: geometry.size.width / 2)
                    if active && valid {
                        Circle().fill(value < 0 && title == "Correlation" ? .orange : .mint)
                            .frame(width: 8, height: 8)
                            .offset(x: max(0, (geometry.size.width - 8) * CGFloat((value + 1) / 2)))
                    }
                }
            }.frame(height: 12)
            HStack { Text(left); Spacer(); Text("0"); Spacer(); Text(right) }.foregroundStyle(.secondary)
        }
    }

    var body: some View {
        let v = metrics.meterValues
        HStack(alignment: .top, spacing: 28) {
            VStack(spacing: 8) {
                Text("RMS / Peak · dBFS · −60 … +6").foregroundStyle(.secondary)
                meter("L", rms: v[2], peak: metrics.leftPeak)
                meter("R", rms: v[3], peak: metrics.rightPeak)
                meter("M", rms: v[4], peak: v[0])
                meter("S", rms: v[5], peak: v[1])
            }.frame(maxWidth: .infinity)
            VStack(spacing: 10) {
                scale("Correlation", value: metrics.correlation, left: "−1", right: "+1", valid: metrics.correlationValid)
                Text(active && metrics.correlationValid ? String(format: "Rolling average (1 s): %.2f", metrics.correlationAverage) : "Rolling average: —")
                    .foregroundStyle(.secondary)
            }.frame(maxWidth: .infinity)
            VStack(spacing: 10) {
                scale("Energy balance", value: v[6], left: "L", right: "R")
                Text(active ? String(format: "Side energy: %.1f %%", 100 * v[7]) : "Side energy: —")
                Text("M = (L+R)/√2 · S = (L−R)/√2").foregroundStyle(.secondary)
            }.frame(maxWidth: .infinity)
        }.font(.caption.monospacedDigit())
    }
}
