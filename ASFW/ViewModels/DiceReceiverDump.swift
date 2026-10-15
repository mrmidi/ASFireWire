// Driver-side DICE receiver dump (selectors 1016/1017). Decoding never reads
// a device register; the driver performed the reads and the safety gating.
import Foundation

struct DiceReceiverDump: Equatable {
    static let wireBytes = 248

    let guid: UInt64
    let generation: UInt32
    let phase: UInt32
    let refusal: UInt32
    let failedStage: UInt32
    let lastError: UInt32
    let asicType: UInt32
    let trigger: UInt32
    let count: UInt32
    let validMask: UInt32
    let flags: UInt32
    let entries: [(address: UInt32, value: UInt32)]

    static func == (lhs: Self, rhs: Self) -> Bool {
        lhs.guid == rhs.guid && lhs.generation == rhs.generation && lhs.phase == rhs.phase &&
            lhs.refusal == rhs.refusal && lhs.failedStage == rhs.failedStage &&
            lhs.lastError == rhs.lastError && lhs.count == rhs.count && lhs.validMask == rhs.validMask &&
            lhs.entries.map(\.address) == rhs.entries.map(\.address) &&
            lhs.entries.map(\.value) == rhs.entries.map(\.value)
    }

    static func decode(_ data: Data) -> Self? {
        guard data.count >= wireBytes else { return nil }
        func u32(_ offset: Int) -> UInt32 {
            data.withUnsafeBytes { UInt32(littleEndian: $0.loadUnaligned(fromByteOffset: offset, as: UInt32.self)) }
        }
        guard u32(0) == 1, Int(u32(4)) == wireBytes, u32(20) <= 5, u32(44) <= 24 else { return nil }
        let guid = data.withUnsafeBytes {
            UInt64(littleEndian: $0.loadUnaligned(fromByteOffset: 8, as: UInt64.self))
        }
        let count = Int(u32(44))
        return Self(guid: guid, generation: u32(16), phase: u32(20), refusal: u32(24),
                    failedStage: u32(28), lastError: u32(32), asicType: u32(36), trigger: u32(40),
                    count: u32(44), validMask: u32(48), flags: u32(52),
                    entries: (0..<count).map { (u32(56 + $0 * 8), u32(60 + $0 * 8)) })
    }

    var isRunning: Bool { phase == 1 }
    var isComplete: Bool { phase == 2 }

    /// Address → value for every entry the driver actually read.
    var values: [UInt32: UInt32] {
        var result: [UInt32: UInt32] = [:]
        for (index, entry) in entries.enumerated() where validMask & (1 << UInt32(index)) != 0 {
            result[entry.address] = entry.value
        }
        return result
    }

    var outcome: String {
        switch phase {
        case 0: return "No receiver dump has run."
        case 1: return "Receiver dump running."
        case 2: return "Receiver dump complete (\(count) registers)."
        case 3:
            let reasons = ["", "no verified PEEK firmware for this device",
                           "no TCAT extension, so the chip cannot be identified",
                           "the chip is not a TCD2210/TCD2220 (ASIC \(asicType))",
                           "no firmware-loader mailbox", "the firmware-loader mailbox is busy",
                           "the driver has no timer scheduler"]
            let reason = Int(refusal) < reasons.count ? reasons[Int(refusal)] : "unknown reason \(refusal)"
            return "Driver refused the receiver dump: \(reason)."
        case 4:
            let stages = ["none", "sections", "caps", "probe", "write-params", "verify-params",
                          "execute", "poll", "status", "value"]
            let stage = Int(failedStage) < stages.count ? stages[Int(failedStage)] : "\(failedStage)"
            return String(format: "Receiver dump failed at %@ (0x%08X) after %d of %d registers.",
                          stage, lastError, validMask.nonzeroBitCount, count)
        case 5: return "Receiver dump cancelled."
        default: return "Unknown receiver dump state \(phase)."
        }
    }
}
