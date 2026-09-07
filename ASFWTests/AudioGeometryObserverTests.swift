import Testing
@testable import ASFW

// These two observers turn monotonic counters into the only live numbers on the
// Audio Geometry panel. Every failure mode here is silent — a wrong rate looks
// exactly like a right one — so the differencing rules are pinned individually.
@MainActor
struct AudioGeometryObserverTests {

    // MARK: - Cadence

    // One reading is not a rate. The panel must say "sampling" rather than
    // invent a number from a single counter value.
    @Test func firstSampleProducesNoRate() {
        var o = AudioCadenceObserver()
        o.sample(txPackets: 1000, rxPackets: 1000, atUptime: 10)
        #expect(o.txPacketsPerSecond == nil)
        #expect(o.rxPacketsPerSecond == nil)
        #expect(o.samples == 0)
    }

    // A healthy stream advances both cursors at the isochronous cycle rate.
    @Test func steadyStreamReportsTheCycleRate() {
        var o = AudioCadenceObserver()
        o.sample(txPackets: 0, rxPackets: 0, atUptime: 10)
        o.sample(txPackets: 8000, rxPackets: 8000, atUptime: 11)
        #expect(o.txPacketsPerSecond == 8000)
        #expect(o.rxPacketsPerSecond == 8000)
        #expect(o.samples == 1)

        // Packets to interrupts is a division by the completion group, and the
        // direction matters: inverting it would report 8000×6 instead of 8000/6.
        let irq = o.interruptsPerSecond(packetsPerGroup: 6,
                                        packetRate: o.txPacketsPerSecond)
        #expect(irq != nil)
        #expect(abs(irq! - 1333.3333) < 0.001)
    }

    // A stalled interrupt path leaves the cursor frozen. Zero is a real,
    // reportable answer — not missing data — because it is the signature the
    // panel exists to surface.
    @Test func frozenCursorReportsZeroNotNil() {
        var o = AudioCadenceObserver()
        o.sample(txPackets: 4242, rxPackets: 4242, atUptime: 10)
        o.sample(txPackets: 4242, rxPackets: 4242, atUptime: 11)
        #expect(o.txPacketsPerSecond == 0)
        #expect(o.rxPacketsPerSecond == 0)
    }

    // The driver zeroes these counters when a binding is re-established. That
    // is not a negative rate and not a 64-bit wrap.
    @Test func counterResetSuppressesTheRateRatherThanInventingOne() {
        var o = AudioCadenceObserver()
        o.sample(txPackets: 100_000, rxPackets: 100_000, atUptime: 10)
        o.sample(txPackets: 0, rxPackets: 0, atUptime: 11)
        #expect(o.txPacketsPerSecond == nil)
        #expect(o.rxPacketsPerSecond == nil)

        // And it re-baselines cleanly from the new origin.
        o.sample(txPackets: 8000, rxPackets: 8000, atUptime: 12)
        #expect(o.txPacketsPerSecond == 8000)
    }

    // A suspended app or a slept machine makes a large counter delta look like
    // a huge rate. Both ends of the usable window are refused.
    @Test func unusableIntervalsAreRefused() {
        var tooLong = AudioCadenceObserver()
        tooLong.sample(txPackets: 0, rxPackets: 0, atUptime: 10)
        tooLong.sample(txPackets: 8_000_000, rxPackets: 8_000_000, atUptime: 1010)
        #expect(tooLong.txPacketsPerSecond == nil)
        #expect(tooLong.samples == 0)

        var tooShort = AudioCadenceObserver()
        tooShort.sample(txPackets: 0, rxPackets: 0, atUptime: 10)
        tooShort.sample(txPackets: 8, rxPackets: 8, atUptime: 10.01)
        #expect(tooShort.txPacketsPerSecond == nil)
    }

    // Directions are independent: TX can stall while RX keeps running, and that
    // asymmetry is diagnostic. It must not be collapsed into one verdict.
    @Test func directionsAreReportedIndependently() {
        var o = AudioCadenceObserver()
        o.sample(txPackets: 0, rxPackets: 0, atUptime: 10)
        o.sample(txPackets: 0, rxPackets: 8000, atUptime: 11)
        #expect(o.txPacketsPerSecond == 0)
        #expect(o.rxPacketsPerSecond == 8000)
    }

    @Test func interruptRateNeedsAGroupSize() {
        let o = AudioCadenceObserver()
        #expect(o.interruptsPerSecond(packetsPerGroup: 0, packetRate: 8000) == nil)
        #expect(o.interruptsPerSecond(packetsPerGroup: 6, packetRate: nil) == nil)
    }

    // MARK: - Margin

    // First contact skips the interval already on the wire. Nothing says when
    // it completed or under which geometry, so folding it would attribute an
    // unknown past to the setting currently displayed.
    @Test func firstContactSkipsTheIntervalAlreadyOnTheWire() {
        var o = AudioMarginObserver()
        o.sample(appliedSequence: 1, intervalSequence: 2, intervalMinimum: 7)
        #expect(o.didResetOnLastSample)
        #expect(o.worstIntervalMarginPackets == nil)
        #expect(o.intervalsObserved == 0)

        o.sample(appliedSequence: 1, intervalSequence: 4, intervalMinimum: 7)
        #expect(!o.didResetOnLastSample)
        #expect(o.worstIntervalMarginPackets == 7)
        #expect(o.intervalsObserved == 1)
    }

    // The whole point: a margin observed under the previous geometry must not
    // be shown beside the new one.
    @Test func applyingNewGeometryDiscardsTheOldHistory() {
        var o = AudioMarginObserver()
        o.sample(appliedSequence: 1, intervalSequence: 2, intervalMinimum: 40)
        o.sample(appliedSequence: 1, intervalSequence: 4, intervalMinimum: 40)
        o.sample(appliedSequence: 1, intervalSequence: 6, intervalMinimum: 12)
        #expect(o.worstIntervalMarginPackets == 12)
        #expect(o.intervalsObserved == 2)

        // Telemetry lags the apply, so the interval published at the moment the
        // epoch moves belongs to the OLD geometry and is skipped like a first
        // contact — not folded into the new epoch.
        o.sample(appliedSequence: 2, intervalSequence: 8, intervalMinimum: 90)
        #expect(o.didResetOnLastSample)
        #expect(o.worstIntervalMarginPackets == nil)
        #expect(o.intervalsObserved == 0)

        o.sample(appliedSequence: 2, intervalSequence: 10, intervalMinimum: 90)
        #expect(o.worstIntervalMarginPackets == 90)
        #expect(o.intervalsObserved == 1)
    }

    // The panel polls at 1 Hz; intervals complete on their own schedule. Reading
    // the same completed interval twice must not look like two observations.
    @Test func repeatedPollsOfOneIntervalCountOnce() {
        var o = AudioMarginObserver()
        o.sample(appliedSequence: 1, intervalSequence: 2, intervalMinimum: 30)
        o.sample(appliedSequence: 1, intervalSequence: 4, intervalMinimum: 30)
        o.sample(appliedSequence: 1, intervalSequence: 4, intervalMinimum: 30)
        o.sample(appliedSequence: 1, intervalSequence: 4, intervalMinimum: 30)
        #expect(o.intervalsObserved == 1)
        #expect(o.worstIntervalMarginPackets == 30)
    }

    // It is a running worst case, so a later good interval must not erase an
    // earlier bad one — that is exactly the excursion worth knowing about.
    @Test func worstCaseSurvivesLaterHealthyIntervals() {
        var o = AudioMarginObserver()
        o.sample(appliedSequence: 1, intervalSequence: 2, intervalMinimum: 96)
        o.sample(appliedSequence: 1, intervalSequence: 4, intervalMinimum: 96)
        o.sample(appliedSequence: 1, intervalSequence: 6, intervalMinimum: 14)
        o.sample(appliedSequence: 1, intervalSequence: 8, intervalMinimum: 96)
        #expect(o.worstIntervalMarginPackets == 14)
        #expect(o.intervalsObserved == 3)
    }

    // An unmeasured interval advances the sequence but carries no value. It
    // must not be folded in as a zero, which would read as a holed ring.
    @Test func unmeasuredIntervalsDoNotCountAsZeroMargin() {
        var o = AudioMarginObserver()
        o.sample(appliedSequence: 1, intervalSequence: 2, intervalMinimum: nil)
        o.sample(appliedSequence: 1, intervalSequence: 4, intervalMinimum: nil)
        #expect(o.worstIntervalMarginPackets == nil)
        #expect(o.intervalsObserved == 0)

        o.sample(appliedSequence: 1, intervalSequence: 6, intervalMinimum: 50)
        #expect(o.worstIntervalMarginPackets == 50)
        #expect(o.intervalsObserved == 1)
    }

    // The ratio against the hardware floor is what turns a packet count into a
    // verdict; below 1.0 the descriptor ring was holed.
    @Test func marginRatioAgainstTheFatalFloor() {
        var o = AudioMarginObserver()
        o.sample(appliedSequence: 1, intervalSequence: 2, intervalMinimum: 96)
        #expect(o.marginOverFloor(hardwareFloorPackets: 48) == nil)

        o.sample(appliedSequence: 1, intervalSequence: 4, intervalMinimum: 96)
        #expect(o.marginOverFloor(hardwareFloorPackets: 48) == 2.0)

        o.sample(appliedSequence: 1, intervalSequence: 6, intervalMinimum: 24)
        #expect(o.marginOverFloor(hardwareFloorPackets: 48) == 0.5)

        // No floor published means no verdict, not a division by zero.
        #expect(o.marginOverFloor(hardwareFloorPackets: 0) == nil)
    }
}
