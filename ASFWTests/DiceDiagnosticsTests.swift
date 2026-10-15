import Foundation
import Testing
@testable import ASFW

@MainActor
struct DiceDiagnosticsTests {
    @Test func decodesObservedWorkingReceiverAndActiveLowMute() throws {
        let values = DiceReceiverDiagnostics.decode(ado: 0xF800FFFF, faults: 0)
        #expect(values.first?.value == "Unmuted, locked")
        #expect(values.first { $0.id == "Lock count" }?.raw == 65535)
        #expect(DiceReceiverDiagnostics.decode(ado: 0, faults: 0).first?.value == "Muted, unlocked")
        let events = DiceReceiverDiagnostics.decode(ado: 0, faults: 0x10000002)
        #expect(events.first { $0.id == "ARX1 latched events" }?.value == "DBC failure")
        #expect(events.first { $0.id == "ARX2 latched events" }?.value == "slip")
    }

    @Test func standardCaptureUsesPublishedOffsetsAndPreservesSlipHistory() async throws {
        let bus = DiceDiagnosticBus()
        let values = try await bus.reader().capture(hardware: false)
        #expect(values.first { $0.id == "GLOBAL_ENABLE" }?.raw == 1)
        #expect(values.first { $0.id == "Clock selection" }?.value == "48000 Hz · internal")
        #expect(values.first { $0.id == "rx0.ISO_CHANNEL" }?.value == "Unassigned")
        #expect(values.first { $0.id == "tx0.SPEED" }?.value == "S800")
        #expect(bus.dumpRequests.isEmpty)
        #expect(!bus.reads.contains { $0.0 <= bus.global + 0x58 && $0.0 + UInt64($0.1) > bus.global + 0x58 })
    }

    @Test func rejectsResetDuringReadBeforeAskingTheDriver() async {
        let bus = DiceDiagnosticBus()
        bus.resetAfterRead = true
        await #expect(throws: NSError.self) { try await bus.reader().capture(hardware: true) }
        #expect(bus.dumpRequests.isEmpty)
    }

    @Test func rejectsInvalidGeometryWithoutWalkingIt() async {
        let bus = DiceDiagnosticBus()
        bus.put(DiceDiagnosticBus.base + 0x100, [UInt32.max, 4])
        await #expect(throws: NSError.self) { try await bus.reader().capture(hardware: false) }
        #expect(bus.reads.count == 4)
    }

    @Test func driverRefusalIsReportedNotDecoded() async {
        let bus = DiceDiagnosticBus()
        bus.dumpError = NSError(domain: "Driver", code: 3)
        await #expect(throws: NSError.self) { try await bus.reader().capture(hardware: true) }
        #expect(bus.dumpRequests == [false])
    }

    @Test func hardwareCaptureDecodesTheDriverDump() async throws {
        let bus = DiceDiagnosticBus()
        let values = try await bus.reader().capture(hardware: true)
        #expect(values.first { $0.id == "ARX1_ADO_CFG" }?.raw == 0xF800FFFF)
        #expect(bus.dumpRequests == [false])
        #expect(bus.writes.isEmpty) // the app never writes to the device
    }

    @Test func missingDumpRegisterFailsInsteadOfDecodingZero() async {
        let bus = DiceDiagnosticBus()
        bus.dumpOmits = 0xCF000144
        await #expect(throws: NSError.self) { try await bus.reader().capture(hardware: true) }
    }

    @Test func decodesCipAndInclusiveFifoCapacity() {
        let values = DiceReceiverDiagnostics.decodeStream(prefix: "ARX1", cfg0: 0x80000082,
            cfg1: 0, cip0: 0x011000AB, cip1: 0x90021234)
        #expect(values.first { $0.id == "ARX1_CFG0" }?.value.contains("FIFO partition 2") == true)
        #expect(values.first { $0.id == "ARX1_CIP0" }?.value.contains("DBS 16") == true)
        #expect(values.first { $0.id == "ARX1_CIP0" }?.value.contains("DBC 171") == true)
        #expect(values.first { $0.id == "ARX1_CIP1" }?.value.contains("SYT 0x1234") == true)
        let fifo = DiceReceiverDiagnostics.decodePartition(index: 0, base: 128, limit: 255)
        #expect(fifo.last?.raw == 128)
        #expect(DiceReceiverDiagnostics.decodePartition(index: 0, base: 256, limit: 255).last?.value == "Invalid range")
        #expect(DiceReceiverDiagnostics.decodeFIFOEvents(9).first?.value == "partition 0 underflow, partition 1 overflow")
    }

    @Test func readToClearFifoEventsAreRequestedOnlyWhenOptedIn() async throws {
        let normal = DiceDiagnosticBus()
        let plain = try await normal.reader().capture(hardware: true)
        let optedIn = DiceDiagnosticBus()
        let fifo = try await optedIn.reader().capture(hardware: true, fifoEvents: true)
        #expect(normal.dumpRequests == [false])
        #expect(optedIn.dumpRequests == [true])
        #expect(plain.count + 1 == fifo.count)
    }

    @Test func baselineDoesNotCompareDifferentDevices() {
        let field = DiceDiagnosticValue(id: "GLOBAL_ENABLE", group: "Clock", value: "Enabled", raw: 1)
        let a = DiceDiagnosticsSnapshot(guid: 1, generation: 1, deviceName: "A", capturedAt: Date(),
                                        includesHardware: false, values: [field])
        let b = DiceDiagnosticsSnapshot(guid: 2, generation: 1, deviceName: "B", capturedAt: Date(),
                                        includesHardware: false, values: [field])
        #expect(a.baselineValue(for: field, baseline: b) == nil)
        #expect(a.baselineValue(for: field, baseline: a) == field)
    }
}

@MainActor
private final class DiceDiagnosticBus {
    static let base: UInt64 = 0xFFFFE0000000
    let global = base + 0x80
    var memory: [UInt64: UInt8] = [:]
    var reads: [(UInt64, UInt32)] = []
    var writes: [(UInt64, [UInt8])] = []
    var resetAfterRead = false
    var dumpRequests: [Bool] = []
    var dumpError: Error?
    var dumpOmits: UInt32?
    private var reset = false

    init() {
        put(Self.base, [0x20, 0x18, 0x40, 6, 0x48, 6, 0, 0, 0, 0])
        put(global + 0x4C, [0x20C, 1, 0x201])
        put(global + 0x5C, [48000])
        put(Self.base + 0x100, [1, 4, 3, 16, 1, 3])
        put(Self.base + 0x120, [1, 4, UInt32.max, 0, 8, 1])
    }

    /// The TCD22xx registers DiceReceiverDump.hpp reads, as the driver returns them.
    func driverDump(fifoEvents: Bool) -> [UInt32: UInt32] {
        var registers: [UInt32: UInt32] = [0xCF00013C: 0, 0xCF000144: 0]
        for base: UInt32 in [0xCF000000, 0xCF000030] {
            for offset: UInt32 in [0, 4, 0x20, 0x24] { registers[base + offset] = 0 }
            registers[base + 0x28] = base == 0xCF000000 ? 0xF800FFFF : 0
        }
        for index: UInt32 in 0..<4 {
            registers[0xCF000184 + index * 12] = 0
            registers[0xCF000188 + index * 12] = 0
        }
        if fifoEvents { registers[0xCF0001FC] = 0 }
        if let omitted = dumpOmits { registers[omitted] = nil }
        return registers
    }

    func put(_ address: UInt64, _ words: [UInt32]) {
        let bytes = words.flatMap { value in
            (0..<4).map { UInt8(truncatingIfNeeded: value >> (24 - $0 * 8)) }
        }
        for (offset, value) in bytes.enumerated() { memory[address + UInt64(offset)] = value }
    }

    func reader() -> DiceDiagnosticsReader {
        DiceDiagnosticsReader(read: { address, length in
            self.reads.append((address, length))
            let data = Data((0..<length).map { self.memory[address + UInt64($0)] ?? 0 })
            if self.resetAfterRead { self.reset = true }
            return data
        }, hardwareDump: { fifoEvents in
            self.dumpRequests.append(fifoEvents)
            if let error = self.dumpError { throw error }
            return self.driverDump(fifoEvents: fifoEvents)
        }, validate: {
            if self.reset { throw NSError(domain: "TestBusReset", code: 1) }
        })
    }
}
