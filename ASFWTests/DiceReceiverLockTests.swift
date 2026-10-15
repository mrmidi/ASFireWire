import Foundation
import Testing
@testable import ASFW

struct DiceReceiverLockTests {
    private func wire(phase: UInt32 = 2, locked: Bool = false) -> Data {
        var data = Data(repeating: 0, count: 96)
        func put(_ offset: Int, _ value: UInt32) {
            var value = value.littleEndian
            withUnsafeBytes(of: &value) { data.replaceSubrange(offset..<(offset + 4), with: $0) }
        }
        put(0, 1); put(4, 96)
        put(8, 0x12345678); put(12, 0xD1CE0000)
        put(16, 7); put(20, phase); put(24, 5000); put(28, 4)
        put(32, 1); put(36, 0xE00002D6); put(40, 0x201)
        put(44, locked ? 0x40 : 0); put(48, 0x00400000)
        put(52, 1); put(56, locked ? 1 : 0); put(60, locked ? 1 : 0)
        for i in 0..<4 {
            put(64 + i * 8, i == 0 ? 3 : UInt32.max)
            put(68 + i * 8, i == 0 && locked ? 120 : UInt32.max)
        }
        return data
    }

    @Test func decodesEveryFieldAndRetainsNeverLockedVerdict() throws {
        let result = try #require(DiceReceiverLock.decode(wire()))
        #expect(result.guid == 0xD1CE000012345678)
        #expect(result.generation == 7)
        #expect(result.phase == 2)
        #expect(result.elapsedMs == 5000)
        #expect(result.samples == 4)
        #expect(result.readErrors == 1)
        #expect(result.lastError == 0xE00002D6)
        #expect(result.status == 0x201)
        #expect(result.extStatus == 0)
        #expect(result.slipHistory == 0x00400000)
        #expect(result.expectedMask == 1)
        #expect(result.observedMask == 0)
        #expect(result.lostMask == 0)
        #expect(result.channels == [3, UInt32.max, UInt32.max, UInt32.max])
        #expect(result.firstLockMs == Array(repeating: UInt32.max, count: 4))
        #expect(result.report.contains("rx0 (ch3): NEVER observed locked in 5000 ms"))
        #expect(result.report.contains("Read failures leave gaps"))
    }

    @Test func lockedAndLostEvidenceReachesDeviceReport() throws {
        var snapshot = DiceSnapshot()
        snapshot.receiverLock = try #require(DiceReceiverLock.decode(wire(locked: true)))
        let report = DiceReportTextFormatter.format(snapshot: snapshot, appVersion: nil)
        #expect(report.contains("first observed locked at +120 ms; lock loss observed"))
        #expect(report.contains("accumulated slip=0x00400000"))
        #expect(report.contains("Generation 7"))
    }

    @Test(arguments: [UInt32(1), 3, 4, 5])
    func incompleteWatchesDoNotClaimNeverLocked(phase: UInt32) throws {
        let result = try #require(DiceReceiverLock.decode(wire(phase: phase)))
        #expect(result.report.contains("NEVER") == false)
    }

    @Test func rejectsTruncationInvalidVersionAndInvalidMask() {
        #expect(DiceReceiverLock.decode(Data(wire().prefix(95))) == nil)
        var data = wire()
        data[0] = 2
        #expect(DiceReceiverLock.decode(data) == nil)
        data = wire()
        data[4] = 97
        #expect(DiceReceiverLock.decode(data) == nil)
        data = wire()
        data[52] = 16
        #expect(DiceReceiverLock.decode(data) == nil)
    }
}
