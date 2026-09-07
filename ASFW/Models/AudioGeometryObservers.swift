import Foundation

// Observed counterparts to the published geometry.
//
// Everything on the Audio Geometry panel is otherwise static: constants, or
// functions of the sample rate. These two observers are the only state the app
// keeps that the driver does not, and they exist because a *rate* and a
// *worst case since an event* cannot be read from one snapshot — both need two
// samples and the elapsed time between them, and neither is on the wire.
//
// Both are plain value types with no SwiftUI or IOKit dependency so the
// differencing rules (counter resets, implausible gaps, epoch changes) are
// testable without a driver.

/// Turns the driver's monotonic packet cursors into observed rates.
///
/// The counters are in **packets**, not interrupts: `txTransportCompletionCursor`
/// is the transport's completion cursor, which the driver subtracts from
/// `txTransportCommittedEnd` to get a margin in packets. One interrupt covers
/// one completion group, so interrupts are packets ÷ group size — doing that
/// division in the wrong direction would misreport the cadence by 6×.
struct AudioCadenceObserver: Equatable {
    /// A gap longer than this means the samples are not comparable — the app
    /// was suspended, the machine slept, or the panel was off screen. Re-baseline
    /// instead of dividing a large counter delta by a small elapsed time.
    static let maximumUsableGapSeconds: Double = 10
    /// Below this the quantisation of a 1 Hz poll dominates the answer.
    static let minimumUsableGapSeconds: Double = 0.25

    private var lastTxPackets: UInt64?
    private var lastRxPackets: UInt64?
    private var lastSampledAt: Double?

    private(set) var txPacketsPerSecond: Double?
    private(set) var rxPacketsPerSecond: Double?

    /// Number of samples folded since the last re-baseline. The panel uses it
    /// to say "waiting" rather than showing a rate derived from one reading.
    private(set) var samples: Int = 0

    mutating func reset() {
        self = AudioCadenceObserver()
    }

    mutating func sample(txPackets: UInt64, rxPackets: UInt64, atUptime now: Double) {
        defer {
            lastTxPackets = txPackets
            lastRxPackets = rxPackets
            lastSampledAt = now
        }
        guard let previousAt = lastSampledAt,
              let previousTx = lastTxPackets,
              let previousRx = lastRxPackets else { return }

        let elapsed = now - previousAt
        guard elapsed >= Self.minimumUsableGapSeconds,
              elapsed <= Self.maximumUsableGapSeconds else {
            // Not a usable interval. Drop the rates rather than reporting a
            // stale one as if it were current.
            txPacketsPerSecond = nil
            rxPacketsPerSecond = nil
            samples = 0
            return
        }

        txPacketsPerSecond = Self.rate(from: previousTx, to: txPackets, over: elapsed)
        rxPacketsPerSecond = Self.rate(from: previousRx, to: rxPackets, over: elapsed)
        samples += 1
    }

    /// A counter that went backwards was reset by the driver on rebind. That is
    /// not a negative rate and not a wrap — report nothing until the next pair.
    private static func rate(from previous: UInt64, to current: UInt64,
                             over elapsed: Double) -> Double? {
        guard current >= previous else { return nil }
        return Double(current - previous) / elapsed
    }

    func interruptsPerSecond(packetsPerGroup: UInt32, packetRate: Double?) -> Double? {
        guard let packetRate, packetsPerGroup != 0 else { return nil }
        return packetRate / Double(packetsPerGroup)
    }
}

/// Worst committed margin observed since the geometry currently in force took
/// effect.
///
/// The driver publishes a lifetime minimum, but that one spans every apply the
/// dext has ever seen, so it cannot answer the only question the tuning control
/// raises: *did the value I just set improve anything?* This accumulates the
/// per-interval minima and throws them away whenever `appliedSequence` moves,
/// so the number beside the picker always belongs to the setting beside it.
struct AudioMarginObserver: Equatable {
    private var epoch: UInt32?
    private var lastIntervalSequence: UInt64?

    private(set) var worstIntervalMarginPackets: UInt32?
    private(set) var intervalsObserved: Int = 0
    /// True when the epoch moved on the most recent sample, i.e. the history
    /// shown belongs to a geometry that is no longer in force.
    private(set) var didResetOnLastSample = false

    mutating func reset() {
        self = AudioMarginObserver()
    }

    /// - Parameters:
    ///   - appliedSequence: the driver's apply counter; any change discards history.
    ///   - intervalSequence: the telemetry seqlock. It is bumped twice per
    ///     completed interval and published only on an even, stable value, so
    ///     it is used purely as a change detector — never as a count of
    ///     anything, and never as an interrupt rate.
    ///   - intervalMinimum: the completed interval's minimum committed margin,
    ///     nil when the driver reports it as not measured.
    mutating func sample(appliedSequence: UInt32, intervalSequence: UInt64,
                         intervalMinimum: UInt32?) {
        didResetOnLastSample = false
        if epoch != appliedSequence {
            epoch = appliedSequence
            // Adopt the currently published interval as already seen rather
            // than clearing it. Telemetry lags the apply, so the interval on
            // the wire at this moment completed under the PREVIOUS geometry;
            // folding it would attribute the old setting's margin to the new
            // one, which is the whole failure this gate exists to prevent.
            lastIntervalSequence = intervalSequence
            worstIntervalMarginPackets = nil
            intervalsObserved = 0
            didResetOnLastSample = true
            return
        }
        // Only a genuinely new interval counts. At a 1 Hz poll the same
        // completed interval is otherwise read repeatedly and inflates the
        // sample count without adding evidence.
        guard intervalSequence != lastIntervalSequence else { return }
        lastIntervalSequence = intervalSequence
        guard let intervalMinimum else { return }
        intervalsObserved += 1
        worstIntervalMarginPackets = min(
            worstIntervalMarginPackets ?? intervalMinimum, intervalMinimum)
    }

    /// How close the worst observation came to the floor at which the transport
    /// fails. Below 1.0 means the ring was holed.
    func marginOverFloor(hardwareFloorPackets: UInt32) -> Double? {
        guard let worst = worstIntervalMarginPackets, hardwareFloorPackets != 0
        else { return nil }
        return Double(worst) / Double(hardwareFloorPackets)
    }
}
