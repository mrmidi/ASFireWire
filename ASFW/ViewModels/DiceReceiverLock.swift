// Cached driver telemetry; decoding this never reads a device register.
import Foundation

struct DiceReceiverLock: Equatable {
    let guid: UInt64
    let generation: UInt32
    let phase: UInt32
    let elapsedMs: UInt32
    let samples: UInt32
    let readErrors: UInt32
    let lastError: UInt32
    let status: UInt32
    let extStatus: UInt32
    let slipHistory: UInt32
    let expectedMask: UInt32
    let observedMask: UInt32
    let lostMask: UInt32
    let channels: [UInt32]
    let firstLockMs: [UInt32]

    static func decode(_ data: Data) -> Self? {
        guard data.count >= 96 else { return nil }
        func u32(_ offset: Int) -> UInt32 {
            data.withUnsafeBytes { UInt32(littleEndian: $0.loadUnaligned(fromByteOffset: offset, as: UInt32.self)) }
        }
        guard u32(0) == 1, u32(4) >= 96, Int(u32(4)) <= data.count,
              u32(20) <= 5, u32(52) & ~UInt32(0xF) == 0 else { return nil }
        let guid = data.withUnsafeBytes {
            UInt64(littleEndian: $0.loadUnaligned(fromByteOffset: 8, as: UInt64.self))
        }
        return Self(guid: guid, generation: u32(16), phase: u32(20),
                    elapsedMs: u32(24), samples: u32(28), readErrors: u32(32),
                    lastError: u32(36), status: u32(40), extStatus: u32(44),
                    slipHistory: u32(48), expectedMask: u32(52), observedMask: u32(56),
                    lostMask: u32(60), channels: (0..<4).map { u32(64 + $0 * 8) },
                    firstLockMs: (0..<4).map { u32(68 + $0 * 8) })
    }

    var phaseLabel: String {
        switch phase {
        case 0: "not started"
        case 1: "watching"
        case 2: "complete"
        case 3: "stopped early"
        case 4: "cancelled by bus reset"
        case 5: "watch unavailable"
        default: "unknown"
        }
    }

    var report: String {
        var lines = ["Generation \(generation); \(phaseLabel); \(elapsedMs) ms; samples=\(samples); read errors=\(readErrors)"]
        for i in 0..<4 where expectedMask & (1 << i) != 0 {
            let verdict: String
            if firstLockMs[i] != UInt32.max {
                verdict = "first observed locked at +\(firstLockMs[i]) ms"
                    + (lostMask & (1 << i) != 0 ? "; lock loss observed" : "")
            } else if phase == 1 {
                verdict = "lock not yet observed"
            } else if phase == 2, samples > 0 {
                verdict = "NEVER observed locked in \(elapsedMs) ms"
            } else {
                verdict = "lock not observed (watch incomplete)"
            }
            lines.append("rx\(i) (ch\(channels[i])): \(verdict)")
        }
        if expectedMask == 0 { lines.append("No assigned RX streams.") }
        lines.append(String(format: "Last STATUS=0x%08X EXT_STATUS=0x%08X; accumulated slip=0x%08X; last error=0x%08X",
                            status, extStatus, slipHistory, lastError))
        lines.append("Times are sparse observations, not exact hardware lock latency.")
        if readErrors > 0 { lines.append("Read failures leave gaps in lock observations.") }
        return lines.joined(separator: "\n")
    }
}
