import Foundation

/// Bounded manual capture. Standard reads avoid EXTENDED_STATUS because a
/// quadlet read clears slip history (Linux dice-interface.h:144–170).
@MainActor
final class DiceDiagnosticsReader {
    typealias Read = (UInt64, UInt32) async throws -> Data
    /// Receiver registers read by the driver (address → value). The driver owns
    /// the firmware-loader mailbox and decides whether this device may be read
    /// (ASFWDriver/Audio/Diagnostics/DICE/DiceReceiverDump.hpp). Argument: FIFO events.
    typealias HardwareDump = (Bool) async throws -> [UInt32: UInt32]
    private let read: Read
    private let hardwareDump: HardwareDump
    private let validate: () throws -> Void
    private var capturing = false

    init(read: @escaping Read, hardwareDump: @escaping HardwareDump, validate: @escaping () throws -> Void) {
        self.read = read
        self.hardwareDump = hardwareDump
        self.validate = validate
    }

    func capture(hardware: Bool, fifoEvents: Bool = false) async throws -> [DiceDiagnosticValue] {
        guard !capturing else { throw failure("A capture is already running.") }
        capturing = true
        defer { capturing = false }
        let base = DiceWire.generalBase
        let tableData = try await bytes(base, 40)
        guard let table = DiceSectionTable.decode(tableData, names: DiceSectionTable.generalNames,
                                                  requiresPresentSection: true),
              let global = table["global"], global.byteSize >= 0x60 else {
            throw failure("Missing or invalid DICE global section.")
        }
        let globalBase = base + UInt64(global.byteOffset)
        let clock = try await bytes(globalBase + 0x4C, 12)
        let select = try word(clock, 0)
        let enable = try word(clock, 4)
        let status = try word(clock, 8)
        let measured = try await quadlet(globalBase + 0x5C)
        var values = [
            field("Clock selection", "Clock", select,
                  "\(DiceClockRate.name((select >> 8) & 0xFF)) Hz · \(DiceClockSource.name(select & 0xFF))"),
            field("GLOBAL_ENABLE", "Clock", enable, enable != 0 ? "Enabled" : "Disabled"),
            field("GLOBAL_STATUS", "Clock", status,
                  "\(status & 1 != 0 ? "Locked" : "Unlocked") · \(DiceClockRate.name((status >> 8) & 0xFF)) Hz"),
            field("Measured rate", "Clock", measured, "\(measured) Hz")
        ]
        // Linux dice-interface.h:231–346: section-relative stream offsets,
        // TX means device→host; RX means host→device.
        for (name, isTx) in [("tx", true), ("rx", false)] {
            guard let section = table[name], section.isPresent else { continue }
            let address = base + UInt64(section.byteOffset)
            guard section.byteSize >= 8 else { throw failure("Truncated \(name) section.") }
            let header = try await bytes(address, 8)
            let count = try word(header, 0)
            let stride = UInt64(try word(header, 4)) * 4
            guard count <= 4, count == 0 || (stride >= 16 && 8 + UInt64(count) * stride <= UInt64(section.byteSize)) else {
                throw failure("Invalid or oversized \(name) stream geometry.")
            }
            for index in 0..<count {
                let data = try await bytes(address + 8 + UInt64(index) * stride, 16)
                let group = "\(isTx ? "Device → host" : "Host → device") · stream \(index + 1)"
                let channel = try word(data, 0)
                let pcm = try word(data, isTx ? 4 : 8)
                let midi = try word(data, isTx ? 8 : 12)
                values.append(field("\(name)\(index).ISO_CHANNEL", group, channel,
                                    channel == UInt32.max ? "Unassigned" : "\(channel)"))
                values.append(field("\(name)\(index).PCM", group, pcm, "\(pcm) channels"))
                values.append(field("\(name)\(index).MIDI", group, midi, "\(midi) ports"))
                let extra = try word(data, isTx ? 12 : 4)
                values.append(field("\(name)\(index).\(isTx ? "SPEED" : "SEQ_START")", group, extra,
                                    isTx ? ["S100", "S200", "S400", "S800"].enumerated().first { $0.offset == Int(extra) }?.element ?? "Unknown" : "\(extra)"))
            }
        }
        if hardware {
            // TCD22xx receiver map (DICE.pdf §6.1, §6.7), read by the driver.
            let registers = try await hardwareDump(fifoEvents)
            try validate()
            func register(_ address: UInt32) throws -> UInt32 {
                guard let value = registers[address] else {
                    throw failure(String(format: "Driver dump is missing register 0x%08X.", address))
                }
                return value
            }
            let faults = try register(0xCF00013C)
            for index in 0..<2 {
                let base: UInt32 = 0xCF000000 + UInt32(index) * 0x30
                let ado = try register(base + 0x28)
                if index == 0 { values += DiceReceiverDiagnostics.decode(ado: ado, faults: faults) }
                else { values += DiceReceiverDiagnostics.decodeADO(ado, prefix: "ARX2") }
                values += DiceReceiverDiagnostics.decodeStream(prefix: "ARX\(index + 1)",
                    cfg0: try register(base), cfg1: try register(base + 4),
                    cip0: try register(base + 0x20), cip1: try register(base + 0x24))
            }
            // Inclusive RAM address ranges; no fill-level register is documented.
            for index in 0..<4 {
                values += DiceReceiverDiagnostics.decodePartition(index: index,
                    base: try register(0xCF000184 + UInt32(index) * 12),
                    limit: try register(0xCF000188 + UInt32(index) * 12))
            }
            values += DiceReceiverDiagnostics.decodeTransmitEvents(try register(0xCF000144))
            // §6.7.5: this register clears on read. Opt-in only.
            if fifoEvents { values += DiceReceiverDiagnostics.decodeFIFOEvents(try register(0xCF0001FC)) }
        }
        try validate()
        return values
    }

    private func bytes(_ address: UInt64, _ length: UInt32) async throws -> Data {
        try Task.checkCancellation()
        try validate()
        let data = try await read(address, length)
        try Task.checkCancellation()
        try validate()
        guard data.count == Int(length) else { throw failure("Short read at \(String(address, radix: 16)).") }
        return data
    }

    private func quadlet(_ address: UInt64) async throws -> UInt32 {
        try word(await bytes(address, 4), 0)
    }

    private func word(_ data: Data, _ offset: Int) throws -> UInt32 {
        guard let value = DiceWire.u32(data, offset) else { throw failure("Truncated register value.") }
        return value
    }


    private func field(_ id: String, _ group: String, _ raw: UInt32, _ value: String) -> DiceDiagnosticValue {
        DiceDiagnosticValue(id: id, group: group, value: value, raw: raw)
    }

    private func failure(_ message: String) -> NSError {
        NSError(domain: "DiceDiagnostics", code: 1, userInfo: [NSLocalizedDescriptionKey: message])
    }
}
