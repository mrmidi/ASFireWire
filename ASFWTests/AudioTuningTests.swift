import Testing
@testable import ASFW

@MainActor
struct AudioTuningTests {
    // The Swift mirror is hand-written against
    // ASFWDriver/UserClient/WireFormats/AudioTuningWireFormats.hpp. A drift here
    // decodes the driver's reply into the wrong fields, which is why the
    // connector refuses a size or version it does not recognise rather than
    // showing the operator whatever landed.
    @Test func wireLayoutMatchesDriver() {
        #expect(MemoryLayout<AudioTuningSnapshotWire>.size == 224)
        #expect(MemoryLayout<AudioTuningRequestWire>.size == 56)
        #expect(MemoryLayout<AudioTuningSnapshotWire>.offset(of: \.endpointId) == 8)
        #expect(MemoryLayout<AudioTuningSnapshotWire>.offset(of: \.framesPerDataPacket) == 84)
        #expect(MemoryLayout<AudioTuningSnapshotWire>.offset(of: \.requestId) == 108)
        #expect(MemoryLayout<AudioTuningSnapshotWire>.offset(of: \.requestStatus) == 112)
        #expect(MemoryLayout<AudioTuningSnapshotWire>.offset(of: \.ready) == 120)
        // The geometry block starts immediately after `ready` and ends with one
        // reserved word; both ends are pinned so an inserted field is caught.
        #expect(MemoryLayout<AudioTuningSnapshotWire>
            .offset(of: \.txDispatchSlackFloorPackets) == 124)
        #expect(MemoryLayout<AudioTuningSnapshotWire>
            .offset(of: \.rxHardwareRingPackets) == 144)
        #expect(MemoryLayout<AudioTuningSnapshotWire>
            .offset(of: \.txInterruptIntervalMicroseconds) == 148)
        #expect(MemoryLayout<AudioTuningSnapshotWire>
            .offset(of: \.maxFramesPerRxInterrupt) == 168)
        #expect(MemoryLayout<AudioTuningSnapshotWire>
            .offset(of: \.completionBatchFrames) == 216)
        #expect(AudioTuningRequestWire().version == 3)
    }

    @Test func declarationArithmeticDoesNotOverflow() {
        var s = AudioTuningSnapshotWire()
        s.outputLatencyFrames = .max; s.inputLatencyFrames = .max
        s.outputSafetyOffsetFrames = .max; s.inputSafetyOffsetFrames = .max
        #expect(AudioTuningPresentation.roundTripFrames(s, io: .max) == UInt64(UInt32.max) * 6)
        #expect(AudioTuningPresentation.outputFrames(s, io: .max) == UInt64(UInt32.max) * 3)
    }

    @Test func installedDuetDeclarationsMatchObservedArithmetic() {
        var s = AudioTuningSnapshotWire()
        s.outputLatencyFrames = 67; s.inputLatencyFrames = 40
        s.outputSafetyOffsetFrames = 50; s.inputSafetyOffsetFrames = 50
        #expect(AudioTuningPresentation.roundTripFrames(s, io: 128) == 463)
        #expect(AudioTuningPresentation.outputFrames(s, io: 128) == 245)
    }

    @Test func refusalsAndAbortsRemainVisible() {
        var s = AudioTuningSnapshotWire()
        s.requestId = 12; s.requestStatus = 5; s.lastError = 123
        #expect(AudioTuningPresentation.status(s).contains("12: Rejected"))
        #expect(AudioTuningPresentation.status(s).contains("0x0000007b"))
        #expect(AudioTuningPresentation.Status.isTerminalFailure(s.requestStatus))
        s.requestStatus = 6
        #expect(AudioTuningPresentation.status(s).contains("Aborted"))
        #expect(AudioTuningPresentation.Status.isTerminalFailure(s.requestStatus))
        // An applied request must not reseed the editor.
        s.requestStatus = 4
        #expect(!AudioTuningPresentation.Status.isTerminalFailure(s.requestStatus))
    }

    // MARK: - Published geometry

    private func shippingSnapshot() -> AudioTuningSnapshotWire {
        var s = AudioTuningSnapshotWire()
        s.sampleRateHz = 48_000
        s.isochCyclesPerSecond = 8_000
        s.microsecondsPerIsochCycle = 125
        s.txPacketsPerGroup = 6
        s.rxPacketsPerGroup = 6
        s.txInterruptIntervalMicroseconds = 750
        s.rxInterruptIntervalMicroseconds = 750
        s.txDispatchSlackPackets = 72
        s.txDispatchSlackDefaultPackets = 72
        s.txDispatchSlackFloorPackets = 72
        s.txOwnershipGuardPackets = 48
        s.clientIoBudgetFrames = 512
        return s
    }

    // The cadence is 8000/6 per second. An integer would report 1333 and
    // understate each context by a third of an interrupt every second, so the
    // driver publishes the interval and the panel takes the reciprocal.
    @Test func interruptRateKeepsItsFraction() {
        let s = shippingSnapshot()
        let tx = AudioTuningPresentation.interruptsPerSecond(
            intervalMicroseconds: s.txInterruptIntervalMicroseconds)
        #expect(tx != nil)
        #expect(abs(tx! - 1333.3333) < 0.001)
        #expect(AudioTuningPresentation.interruptsPerSecond(intervalMicroseconds: 0) == nil)
    }

    // Packets convert to time through the published cycle grid, never a 125 or
    // an 8000 spelled out in the app.
    @Test func packetAndFrameConversionsUsePublishedGrid() {
        let s = shippingSnapshot()
        #expect(AudioTuningPresentation.milliseconds(packets: 48, s) == 6.0)
        #expect(AudioTuningPresentation.milliseconds(packets: 168, s) == 21.0)
        #expect(AudioTuningPresentation.milliseconds(frames: 480, rate: 48_000) == 10.0)
        #expect(AudioTuningPresentation.framesPerCycle(s) == 6.0)

        // A snapshot with no grid yet must not be rendered as if it had one.
        var empty = AudioTuningSnapshotWire()
        #expect(AudioTuningPresentation.milliseconds(packets: 48, empty) == nil)
        #expect(AudioTuningPresentation.framesPerCycle(empty) == nil)
        empty.isochCyclesPerSecond = 8_000
        empty.sampleRateHz = 96_000
        #expect(AudioTuningPresentation.framesPerCycle(empty) == 12.0)
    }

    // The ladder is anchored on the driver's default, in whole completion
    // groups. At the shipping geometry it is the familiar 12/8/6/4/2 groups.
    @Test func slackLadderFollowsTheDriversOwnDefault() {
        let s = shippingSnapshot()
        #expect(AudioTuningPresentation.slackPresets(s) == [72, 48, 36, 24, 12])
        #expect(AudioTuningPresentation.groupsLabel(72, s) == "72 packets · 12 groups")
        #expect(AudioTuningPresentation.groupsLabel(6, s) == "6 packets · 1 group")

        // A driver with a different group size or default moves the ladder with
        // it; the app contributes no constant of its own.
        var wider = s
        wider.txPacketsPerGroup = 8
        wider.txDispatchSlackDefaultPackets = 96
        wider.txDispatchSlackPackets = 96
        #expect(AudioTuningPresentation.slackPresets(wider) == [96, 64, 48, 32, 16])
    }

    // Whatever is in force must stay selectable even when it is off the ladder,
    // or the picker would silently show some other value as the current one.
    @Test func ladderAlwaysContainsTheValueInForce() {
        var s = shippingSnapshot()
        s.txDispatchSlackPackets = 30
        let presets = AudioTuningPresentation.slackPresets(s)
        #expect(presets.contains(30))
        #expect(presets.contains(72))
        #expect(presets == presets.sorted(by: >))
    }

    // The floor comes from the driver. A build that publishes none must not
    // paint every value orange.
    @Test func floorComparisonUsesThePublishedFloor() {
        let s = shippingSnapshot()
        #expect(!AudioTuningPresentation.isBelowFloor(72, s))
        #expect(AudioTuningPresentation.isBelowFloor(71, s))
        #expect(AudioTuningPresentation.isBelowFloor(12, s))

        var noFloor = s
        noFloor.txDispatchSlackFloorPackets = 0
        #expect(!AudioTuningPresentation.isBelowFloor(12, noFloor))
    }

    @Test func preparedTargetMirrorsTheDriverFormula() {
        let s = shippingSnapshot()
        #expect(AudioTuningPresentation.preparedTargetPackets(slack: 72, s) == 120)
        #expect(AudioTuningPresentation.preparedTargetPackets(slack: 12, s) == 60)
    }

    // Warnings are the driver's verdict on the geometry actually installed. The
    // panel used to show only its own guess about the value it was offering,
    // which said nothing after an apply was accepted.
    @Test func warningMaskDecodesToOperatorText() {
        #expect(AudioTuningWarning.labels(0).isEmpty)

        let floor = AudioTuningWarning.labels(AudioTuningWarning.dispatchSlackBelowFloor)
        #expect(floor.count == 1)
        #expect(floor[0].contains("below the asserted floor"))

        let both = AudioTuningWarning.labels(
            AudioTuningWarning.dispatchSlackBelowFloor
                | AudioTuningWarning.sharedSlotRingOverProvisioned)
        #expect(both.count == 2)

        // A driver newer than this app must not have its warnings swallowed.
        let unknown = AudioTuningWarning.labels(1 << 7)
        #expect(unknown.count == 1)
        #expect(unknown[0].contains("0x00000080"))
    }
}
