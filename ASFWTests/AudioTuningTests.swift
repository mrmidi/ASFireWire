import Testing
@testable import ASFW

@MainActor
struct AudioTuningTests {
    @Test func wireLayoutMatchesDriver() {
        #expect(MemoryLayout<AudioTuningSnapshotWire>.size == 128)
        #expect(MemoryLayout<AudioTuningRequestWire>.size == 56)
        #expect(MemoryLayout<AudioTuningSnapshotWire>.offset(of: \.endpointId) == 8)
        #expect(MemoryLayout<AudioTuningSnapshotWire>.offset(of: \.requestId) == 108)
        #expect(MemoryLayout<AudioTuningSnapshotWire>.offset(of: \.requestStatus) == 112)
        #expect(AudioTuningRequestWire().version == 2)
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
        s.requestStatus = 6
        #expect(AudioTuningPresentation.status(s).contains("Aborted"))
    }
}
