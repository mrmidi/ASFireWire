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
        #expect(bus.writes.isEmpty)
        #expect(!bus.reads.contains { $0.0 <= bus.global + 0x58 && $0.0 + UInt64($0.1) > bus.global + 0x58 })
    }

    @Test func rejectsResetDuringReadBeforePublishingOrWriting() async {
        let bus = DiceDiagnosticBus()
        bus.resetAfterRead = true
        await #expect(throws: NSError.self) { try await bus.reader().capture(hardware: true) }
        #expect(bus.writes.isEmpty)
    }

    @Test func rejectsInvalidGeometryWithoutWalkingIt() async {
        let bus = DiceDiagnosticBus()
        bus.put(DiceDiagnosticBus.base + 0x100, [UInt32.max, 4])
        await #expect(throws: NSError.self) { try await bus.reader().capture(hardware: false) }
        #expect(bus.reads.count == 4)
    }

    #if DEBUG
    @Test func busyMailboxRefusesWithoutAnyWrite() async {
        let bus = DiceDiagnosticBus()
        bus.put(DiceDiagnosticBus.mailbox + 4, [0x80000008])
        await #expect(throws: NSError.self) { try await bus.reader().capture(hardware: true) }
        #expect(bus.writes.isEmpty)
    }

    @Test func hardwareCaptureUsesOnlyPeekAndChecksFirmwareStatus() async throws {
        let bus = DiceDiagnosticBus()
        let values = try await bus.reader().capture(hardware: true)
        #expect(values.first { $0.id == "ARX1_ADO_CFG" }?.raw == 0xF800FFFF)
        #expect(bus.writes.count == 40)
        for (address, payload) in bus.writes {
            if address == DiceDiagnosticBus.mailbox + 4 {
                #expect(payload == [0x80, 0, 0, 6])
            } else {
                #expect(address == DiceDiagnosticBus.mailbox + 0x2C)
                #expect(Array(payload.prefix(4)) == [0, 0, 0, 2])
                #expect(payload.count == 8)
            }
        }
        let rejected = DiceDiagnosticBus()
        rejected.firmwareStatus = 7
        await #expect(throws: NSError.self) { try await rejected.reader().capture(hardware: true) }
        #expect(rejected.writes.count == 2)
    }
    #endif

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

    #if DEBUG
    @Test func fifoFaultReadIsExplicitAndNeverTouchesFlushRegisters() async throws {
        let normal = DiceDiagnosticBus()
        _ = try await normal.reader().capture(hardware: true)
        let optedIn = DiceDiagnosticBus()
        _ = try await optedIn.reader().capture(hardware: true, fifoEvents: true)
        func targets(_ bus: DiceDiagnosticBus) -> [UInt32] {
            bus.writes.compactMap { address, payload in
                address == DiceDiagnosticBus.mailbox + 0x2C ? DiceWire.u32(Data(payload), 4) : nil
            }
        }
        #expect(!targets(normal).contains(0xCF0001FC))
        #expect(targets(optedIn).contains(0xCF0001FC))
        for address: UInt32 in [0xCF00018C, 0xCF000198, 0xCF0001A4, 0xCF0001B0] {
            #expect(!targets(optedIn).contains(address))
        }
    }
    #endif

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
    static let mailbox: UInt64 = 0xFFFFE0100000
    let global = base + 0x80
    var memory: [UInt64: UInt8] = [:]
    var reads: [(UInt64, UInt32)] = []
    var writes: [(UInt64, [UInt8])] = []
    var resetAfterRead = false
    var firmwareStatus: UInt32 = 0
    private var reset = false

    init() {
        put(Self.base, [0x20, 0x18, 0x40, 6, 0x48, 6, 0, 0, 0, 0])
        put(global + 0x4C, [0x20C, 1, 0x201])
        put(global + 0x5C, [48000])
        put(Self.base + 0x100, [1, 4, 3, 16, 1, 3])
        put(Self.base + 0x120, [1, 4, UInt32.max, 0, 8, 1])
        put(Self.mailbox + 4, [0])
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
        }, write: { address, payload in
            self.writes.append((address, payload))
            for (offset, value) in payload.enumerated() { self.memory[address + UInt64(offset)] = value }
            if address == Self.mailbox + 4 {
                let target = Data((0..<4).map { self.memory[Self.mailbox + 0x30 + UInt64($0)] ?? 0 })
                self.put(Self.mailbox + 4, [6])
                self.put(Self.mailbox + 8, [self.firmwareStatus])
                self.put(Self.mailbox + 0x2C, [DiceWire.u32(target, 0) == 0xCF000028 ? 0xF800FFFF : 0])
            }
        }, validate: {
            if self.reset { throw NSError(domain: "TestBusReset", code: 1) }
        })
    }
}
