import Foundation

/// Display ballistics only. DSP results, loudness integration, histories and
/// clipping counters never consume these smoothed values.
nonisolated struct AnalyzerDisplaySmoother {
    private var level: Float?
    private var peakLevel: Float = -120
    private var lastTime: Double?

    mutating func update(value: Float, peak: Float, mode: UInt32, now: Double,
                         active: Bool) -> (value: Float, peak: Float) {
        guard active, value.isFinite, peak.isFinite else {
            self = Self()
            return (value, peak)
        }
        let target = mode == 0 ? decibels(value) : value
        let targetPeak = mode == 0 ? decibels(peak) : peak
        guard let previous = level, let lastTime else {
            level = target; peakLevel = targetPeak; self.lastTime = now
            return (value, peak)
        }
        let elapsed = max(0, now - lastTime)
        self.lastTime = now
        let timeConstant: Double = mode == 0 ? (target > previous ? 0.035 : 0.3) : 0.18
        let next = target + (previous - target) * Float(exp(-elapsed / timeConstant))
        level = next
        // Peak markers rise immediately and fall slowly; transient peaks stay
        // visible without changing the reported sample/true-peak measurement.
        peakLevel = max(targetPeak, targetPeak + (peakLevel - targetPeak) * Float(exp(-elapsed / 0.45)))
        return mode == 0 ? (pow(10, next / 20), pow(10, peakLevel / 20)) : (next, peak)
    }

    private func decibels(_ value: Float) -> Float { 20 * log10(max(value, 1e-6)) }
}
