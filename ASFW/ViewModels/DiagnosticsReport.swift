//
//  DiagnosticsReport.swift
//  ASFW
//
//  Text-report builder and shared value formatters for the diagnostics report.
//  Split out of DiagnosticsTextFormatter so the report is assembled section by
//  section (each in its own stack frame) rather than in one ~1300-line function.
//  See DiagnosticsTextFormatter.format for why per-section frames matter: the
//  formatter runs on a small-stacked libdispatch worker (~512 KB) and the
//  snapshot is a multi-KB value type, so a single monolithic frame overflowed
//  the stack (___chkstk_darwin guard-page fault / EXC_BAD_ACCESS on entry).
//

import Foundation

/// Mutable text accumulator with the column/padding helpers the report uses.
/// A reference type so it can be threaded through the section functions without
/// `inout` plumbing or per-call value copies.
final class DiagnosticsReport {
    private(set) var text = ""

    func raw(_ s: String) { text += s }

    func title(_ title: String) {
        text += "\n"
        text += "=== \(title) ===\n"
        text += String(repeating: "─", count: title.count + 8) + "\n"
    }

    // NOTE: Swift's String(format:) does NOT support %s safely — it expects a C char*,
    // but Swift String/[CChar] bridge to objects, so %s runs strlen on an object pointer
    // and crashes (EXC_BAD_ACCESS). All column alignment is done with Swift padding instead.
    static func pad(_ s: String, _ width: Int) -> String {
        s.count >= width ? s : s.padding(toLength: width, withPad: " ", startingAt: 0)
    }

    func row(_ label: String, _ value: Any) {
        text += Self.pad(label + ":", 30) + " " + String(describing: value) + "\n"
    }
}

/// Pure value→String formatters shared across report sections.
enum DiagFormat {
    // Valid bus node IDs are 0..62; 0x3F (63), 0xFF and 0xFFFFFFFF are "no node"/sentinels.
    static func nodeStr(_ nodeVal: UInt32) -> String {
        if nodeVal >= 0x3F {
            return "none"
        }
        return String(format: "node %d / 0x%04X", nodeVal, 0xFFC0 + nodeVal)
    }

    // RoleAction::Kind (ASFWDriver/Bus/Role/RolePolicy.hpp).
    static func roleAction(_ v: UInt32) -> String {
        switch v {
        case 0: return "None (stable)"
        case 1: return "DeferForEvidence"
        case 2: return "EnableLocalCycleMaster"
        case 3: return "EnableRemoteCycleMaster"
        case 4: return "ForceRootAndReset"
        case 5: return "ClearContenderAndDelegate"
        // Diagnostic-only verdict: records that the root is unverified /
        // CMC=0 / non-responsive. The Apple-compatible default does NOT mutate
        // the bus on this — it is informational, not an alarm.
        case 6: return "Root unverified — diagnostic only (no bus action)"
        default: return "Unknown (\(v))"
        }
    }

    // RoleResetFlavor (None/Short/Long).
    static func roleReset(_ v: UInt32) -> String {
        switch v {
        case 0: return "None"
        case 1: return "Short"
        case 2: return "Long"
        default: return "Unknown (\(v))"
        }
    }

    // pwr field per IEEE 1394-2008 self-ID packet 0.
    static func powerClass(_ pc: UInt32) -> String {
        switch pc {
        case 0: return "none"
        case 1: return "self+15W"
        case 2: return "self+30W"
        case 3: return "self+45W"
        case 4: return "bus≤3W"
        case 5: return "reserved"
        case 6: return "bus+3Wlink"
        case 7: return "bus+7Wlink"
        default: return "\(pc)"
        }
    }

    static func uptime(_ ns: UInt64) -> String {
        let totalSec = ns / 1_000_000_000
        let h = totalSec / 3600
        let m = (totalSec % 3600) / 60
        let s = totalSec % 60
        return String(format: "%dh %02dm %02ds", h, m, s)
    }

    static func hex16(_ val: UInt32) -> String { String(format: "0x%04X", val) }
    static func hex32(_ val: UInt32) -> String { String(format: "0x%08X", val) }
    static func hex64(_ val: UInt64) -> String { String(format: "0x%016llX", val) }

    // Four-level speed label (node table / async trace).
    static func speed4(_ s: UInt32) -> String {
        switch s {
        case ASFWDiagSpeedS100.rawValue: return "S100"
        case ASFWDiagSpeedS200.rawValue: return "S200"
        case ASFWDiagSpeedS400.rawValue: return "S400"
        case ASFWDiagSpeedS800.rawValue: return "S800"
        default: return "S?"
        }
    }

    // Extended speed label incl. S1600/S3200 (topology tree edges).
    static func speedExt(_ s: UInt32) -> String {
        switch s {
        case ASFWDiagSpeedS100.rawValue: return "S100"
        case ASFWDiagSpeedS200.rawValue: return "S200"
        case ASFWDiagSpeedS400.rawValue: return "S400"
        case ASFWDiagSpeedS800.rawValue: return "S800"
        case ASFWDiagSpeedS1600.rawValue: return "S1600"
        case ASFWDiagSpeedS3200.rawValue: return "S3200"
        default: return "S?"
        }
    }
}
