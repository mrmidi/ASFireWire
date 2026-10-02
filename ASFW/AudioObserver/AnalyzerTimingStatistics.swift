import Foundation

/// Fixed-size counters: completion callbacks update these under the metric store's lock.
/// Publish only a finished one-second bucket, never every individual GPU result.
nonisolated struct AnalyzerTimingStatistics: Sendable, Equatable {
    private var second: Double?
    private var bucketSum: Double = 0
    private var bucketMinimum: Double?
    private var bucketPeak: Double = 0
    private var bucketCount: UInt64 = 0
    private var sum: Double = 0
    private(set) var count: UInt64 = 0
    private(set) var runFastest: Double?
    private(set) var fastest: Double?
    private(set) var runPeak: Double?
    private(set) var mean: Double?
    private(set) var peak: Double?
    private(set) var rate: Double?
    /// Sum of measured spans in the previous complete second (not hardware utilization).
    var millisecondsPerSecond: Double? {
        guard let mean, let rate else { return nil }
        return mean * rate
    }
    var spanPercent: Double? { millisecondsPerSecond.map { $0 / 10 } }
    var frameBudgetMilliseconds: Double? {
        guard let rate, rate > 0 else { return nil }
        return 1_000 / rate
    }
    var runMean: Double? { count > 0 ? sum / Double(count) : nil }

    mutating func append(milliseconds: Double?, at timestamp: Double) {
        guard let milliseconds, milliseconds.isFinite, milliseconds >= 0, timestamp.isFinite else { return }
        let current = floor(timestamp)
        if let second, current > second {
            mean = bucketCount > 0 ? bucketSum / Double(bucketCount) : nil
            peak = bucketCount > 0 ? bucketPeak : nil
            fastest = bucketMinimum
            rate = current - second == 1 ? Double(bucketCount) : nil
            bucketSum = 0; bucketPeak = 0; bucketCount = 0; bucketMinimum = nil
        }
        if let second, current < second { return }
        second = current
        bucketSum += milliseconds; bucketPeak = max(bucketPeak, milliseconds); bucketCount += 1
        bucketMinimum = min(bucketMinimum ?? milliseconds, milliseconds)
        runFastest = min(runFastest ?? milliseconds, milliseconds)
        sum += milliseconds; count += 1
        runPeak = max(runPeak ?? 0, milliseconds)
    }
}
