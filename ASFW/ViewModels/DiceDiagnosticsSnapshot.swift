import Foundation

struct DiceDiagnosticValue: Identifiable, Equatable {
    let id: String
    let group: String
    let value: String
    let raw: UInt32

    var rawText: String { String(format: "0x%08X", raw) }
}

struct DiceDiagnosticsSnapshot {
    let guid: UInt64
    let generation: UInt32
    let deviceName: String
    let capturedAt: Date
    let includesHardware: Bool
    var clearedFIFOEvents = false
    let values: [DiceDiagnosticValue]

    func baselineValue(for field: DiceDiagnosticValue, baseline: Self?) -> DiceDiagnosticValue? {
        guard let baseline, baseline.guid == guid else { return nil }
        return baseline.values.first { $0.id == field.id }
    }

    var report: String {
        (["DICE Diagnostics — \(deviceName)",
          String(format: "GUID: %016llX; generation: %u", guid, generation),
          "Captured: \(capturedAt.ISO8601Format()); hardware: \(includesHardware)",
          "Sequential reads, not an atomic snapshot. Receiver faults are latched history.",
          "FIFO fault flags read and cleared: \(clearedFIFOEvents). Capacity is in RAM address units; live fill level is not available."]
         + values.map { "[\($0.group)] \($0.id): \($0.value) (\($0.rawText))" }).joined(separator: "\n")
    }
}

// DICE.pdf rev 1.02, §6.1.12 (p263) and §6.6.2 (p288–289).
// Mute_n is active low; APBA flags persist until explicitly cleared.
enum DiceReceiverDiagnostics {
    static func decode(ado: UInt32, faults: UInt32) -> [DiceDiagnosticValue] {
        let receiver = "Receiver hardware"
        let faultNames = ["SYT underflow", "DBC failure", "CIP failure", "configuration failure",
                          "reserved", "missed sync", "incomplete packet", "status error",
                          "aborted packet", "long packet", "stale SYT", "SYT overflow",
                          "slip", "repeat", "lock", "unlock"]
        var result = [
            DiceDiagnosticValue(id: "ARX1_ADO_CFG", group: receiver,
                                value: "\(ado & 0x80000000 != 0 ? "Unmuted" : "Muted"), \(ado & 0x10000000 != 0 ? "locked" : "unlocked")", raw: ado),
            DiceDiagnosticValue(id: "Automatic mute", group: receiver,
                                value: ado & 0x20000000 != 0 ? "Enabled" : "Disabled", raw: (ado >> 29) & 1),
            DiceDiagnosticValue(id: "MIDI", group: receiver,
                                value: ado & 0x40000000 != 0 ? "Enabled" : "Disabled", raw: (ado >> 30) & 1),
            DiceDiagnosticValue(id: "Dual wire", group: receiver,
                                value: ado & 0x08000000 != 0 ? "Enabled" : "Disabled", raw: (ado >> 27) & 1),
            DiceDiagnosticValue(id: "Lock count", group: receiver,
                                value: "\(ado & 0xFFFF)", raw: ado & 0xFFFF)
        ]
        for index in 0..<2 {
            let flags = (faults >> (index * 16)) & 0xFFFF
            let names = faultNames.enumerated().compactMap { bit, name in
                flags & (1 << bit) != 0 ? name : nil
            }
            result.append(DiceDiagnosticValue(id: "ARX\(index + 1) latched events", group: receiver,
                value: names.isEmpty ? "None" : names.joined(separator: ", "), raw: flags))
        }
        result.append(DiceDiagnosticValue(id: "APBA_INT0_STATUS", group: receiver,
                                         value: "DBC failure can latch once during startup", raw: faults))
        return result
    }
}

extension DiceReceiverDiagnostics {
    static func decodeADO(_ ado: UInt32, prefix: String) -> [DiceDiagnosticValue] {
        [.init(id: "\(prefix)_ADO_CFG", group: "Receiver hardware",
               value: "\(ado & 0x80000000 != 0 ? "Unmuted" : "Muted"), \(ado & 0x10000000 != 0 ? "locked" : "unlocked"), auto mute \(ado & 0x20000000 != 0 ? "on" : "off"), lock count \(ado & 0xFFFF)", raw: ado)]
    }

    // DICE.pdf §6.1.2–3, §6.1.10–11. CIP registers hold the most recent
    // received headers; separate PEEKs need not refer to the same packet.
    static func decodeStream(prefix: String, cfg0: UInt32, cfg1: UInt32,
                             cip0: UInt32, cip1: UInt32) -> [DiceDiagnosticValue] {
        let group = "\(prefix) · received stream"
        return [
            .init(id: "\(prefix)_CFG0", group: group,
                  value: "\(cfg0 & 0x80000000 != 0 ? "Enabled" : "Disabled") · channel \(cfg0 & 0x3F) · FIFO partition \((cfg0 >> 6) & 3) · SYT32 mode \(cfg0 & 0x00400000 != 0 ? "on" : "off")", raw: cfg0),
            .init(id: "\(prefix)_CFG1", group: group,
                  value: "DBS \((cfg1 >> 21) & 0xFF) \(cfg1 & 0x80000000 != 0 ? "forced" : "from CIP") · FMT \((cfg1 >> 8) & 0x3F) · FDF \(cfg1 & 0xFF)", raw: cfg1),
            .init(id: "\(prefix)_CIP0", group: group,
                  value: "SID \((cip0 >> 24) & 0x3F) · DBS \((cip0 >> 16) & 0xFF) · FN \((cip0 >> 14) & 3) · QPC \((cip0 >> 11) & 7) · SPH \((cip0 >> 10) & 1) · DBC \(cip0 & 0xFF)", raw: cip0),
            .init(id: "\(prefix)_CIP1", group: group,
                  value: String(format: "FMT %u · FDF 0x%02X · SYT 0x%04X%@", (cip1 >> 24) & 0x3F, (cip1 >> 16) & 0xFF, cip1 & 0xFFFF,
                                cip1 & 0xFFFF == 0xFFFF ? " (no timestamp)" : ""), raw: cip1)
        ]
    }

    // §6.7.2–3: limit is the highest usable address, not an exclusive end.
    static func decodePartition(index: Int, base: UInt32, limit: UInt32) -> [DiceDiagnosticValue] {
        let start = base & 0xFFF
        let end = limit & 0xFFF
        return [
            .init(id: "FIFO\(index)_BASE", group: "Media FIFO allocation", value: "\(start)", raw: base),
            .init(id: "FIFO\(index)_LIMIT", group: "Media FIFO allocation", value: "\(end)", raw: limit),
            .init(id: "FIFO\(index) address span", group: "Media FIFO allocation",
                  value: end >= start ? "\(end - start + 1) RAM addresses (inclusive)" : "Invalid range", raw: end >= start ? end - start + 1 : 0)
        ]
    }

    static func decodeFIFOEvents(_ status: UInt32) -> [DiceDiagnosticValue] {
        let names = (0..<8).flatMap { index -> [String] in
            var events: [String] = []
            if status & (1 << (index * 2)) != 0 { events.append("partition \(index) underflow") }
            if status & (1 << (index * 2 + 1)) != 0 { events.append("partition \(index) overflow") }
            return events
        }
        return [.init(id: "AVSFIFO_STAT (read clears flags)", group: "Media FIFO events",
                      value: names.isEmpty ? "No flags at read time" : names.joined(separator: ", "), raw: status)]
    }

    // §6.6.4: device→host transmit-side latched events.
    static func decodeTransmitEvents(_ status: UInt32) -> [DiceDiagnosticValue] {
        let names = ["reserved", "packet abort", "stale frame", "packet boundary error",
                     "SYT underflow", "SYT overflow", "stream start", "stream end"]
        return (0..<2).map { index in
            let flags = (status >> (index * 8)) & 0xFF
            let events = names.enumerated().compactMap { bit, name in flags & (1 << bit) != 0 ? name : nil }
            return .init(id: "ATX\(index + 1) latched events", group: "Device transmitter",
                         value: events.isEmpty ? "None" : events.joined(separator: ", "), raw: flags)
        }
    }
}
