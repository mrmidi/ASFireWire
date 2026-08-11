//
//  AVCReportTextFormatter.swift
//  ASFW
//
//  Renders an AVCDeviceSnapshot as a plain-text report suitable for pasting
//  into a bug report, and readable enough to drive protocol bring-up from.
//
//  Generic AV/C sections come first, because they apply to every family. A
//  per-family section is appended only when that family was recognised.
//
//  The report states plainly what was asked of the device and what was not.
//  Where geometry is reference-derived rather than queried, it says so -- it is
//  never presented as something the device answered.
//

import Foundation

enum AVCReportTextFormatter {

    /// String(format:) does not honour width specifiers for %@ on macOS, so
    /// columns are padded explicitly.
    static func pad(_ text: String, _ width: Int) -> String {
        text.count >= width ? text
            : text + String(repeating: " ", count: width - text.count)
    }

    static func leftPad(_ text: String, _ width: Int) -> String {
        text.count >= width ? text
            : String(repeating: " ", count: width - text.count) + text
    }

    static func rateLabel(_ hz: UInt32) -> String {
        hz % 1000 == 0 ? "\(hz / 1000) kHz"
                       : String(format: "%.1f kHz", Double(hz) / 1000.0)
    }

    static func version(_ value: UInt32) -> String {
        "\(value >> 24).\((value >> 16) & 0xff).\(value & 0xffff)"
    }

    static func hexDump<C: Collection>(_ bytes: C) -> String where C.Element == UInt8 {
        bytes.map { String(format: "%02X", $0) }.joined(separator: " ")
    }

    /// Emitter handed to family section renderers so they append in the same
    /// style as the generic body.
    struct Emitter {
        let line: (String) -> Void
        let head: (String) -> Void
    }

    static func format(snapshot s: AVCDeviceSnapshot, appVersion: String?,
                       driverVersion: String? = nil) -> String {
        var o = ""
        func line(_ t: String = "") { o += t + "\n" }
        func head(_ t: String) {
            line()
            line(t)
            line(String(repeating: "-", count: max(8, t.count)))
        }

        line("ASFW AV/C DEVICE REPORT")
        line("=======================")
        line("Generated: \(ISO8601DateFormatter().string(from: Date()))")
        if let v = appVersion { line("Report app: \(v)") }
        if let v = driverVersion { line("Driver:     \(v)") }
        line()
        line("Read-only dump of a TA 1394 AV/C audio device. No CONTROL commands")
        line("were sent and no device state was changed.")
        line("Paste it whole into the issue; do not trim sections.")

        // MARK: Identity
        head("IDENTITY")
        line(String(format: "GUID:        0x%016llX", s.guid))
        line("Vendor:      \(s.vendorName)" + String(format: "  (0x%06X)", s.vendorId))
        line("Model:       \(s.modelName)" + String(format: "  (0x%06X)", s.modelId))
        line("Node / gen:  \(s.nodeId) / \(s.generation)")
        let families = s.familyLabels
        line("Family:      " + (families.isEmpty ? "generic AV/C (no vendor family recognised)"
                                                 : families.joined(separator: ", ")))

        // MARK: Probe policy
        head("PROBE POLICY")
        line("Tier:      \(s.policy.tier.label)")
        line("Rationale: \(s.policy.rationale)")
        if s.policy.tier == .memoryOnly {
            line()
            line("This tier exists because some firmware hangs permanently on AV/C")
            line("commands it does not implement, requiring a power cycle to recover.")
            line("The tier is chosen from Config ROM identity before anything is sent.")
        }

        // MARK: Generic AV/C inventory
        head("AV/C UNIT INVENTORY")
        if let p = s.inventory.unitPlugs {
            line("Source: \(s.inventory.unitPlugsProvenance.label)")
            line("Isochronous input plugs:   \(p.isochronousInput)   (host -> device streams)")
            line("Isochronous output plugs:  \(p.isochronousOutput)   (device -> host streams)")
            line("External input plugs:      \(p.externalInput)")
            line("External output plugs:     \(p.externalOutput)")
        } else if s.policy.tier == .memoryOnly {
            line("<not queried — suppressed by probe policy>")
        } else {
            line("<no answer>")
        }

        line()
        if s.inventory.subunits.isEmpty {
            switch s.inventory.subunitsProvenance {
            case .probed: line("Subunits: <none reported>")
            case .driverDiscovery, .notQueried: line("Subunits: <not queried>")
            }
        } else {
            line("Subunits  (\(s.inventory.subunitsProvenance.label))")
            line("  " + pad("type", 8) + pad("id", 5) + pad("src", 6) + pad("dst", 6) + "name")
            for su in s.inventory.subunits {
                line("  " + pad(String(format: "0x%02X", su.type), 8)
                     + pad(String(su.id), 5)
                     + pad(su.sourcePlugs == 0 ? "-" : String(su.sourcePlugs), 6)
                     + pad(su.destinationPlugs == 0 ? "-" : String(su.destinationPlugs), 6)
                     + su.typeName)
            }
        }

        if s.policy.tier == .avcStatus {
            head("MUSIC SUBUNIT")
            if let count = s.inventory.musicSubunitInputPlugCount {
                line("Input plugs: \(count)")
                if s.inventory.musicSubunitInputPlugs.isEmpty {
                    line("  <none enumerated>")
                } else {
                    line("  " + pad("plug", 6) + pad("type", 6) + "name")
                    for plug in s.inventory.musicSubunitInputPlugs {
                        line("  " + pad(String(plug.index), 6)
                             + pad(String(format: "0x%02X", plug.type), 6)
                             + plug.typeName)
                    }
                }
            } else {
                line("<not reported — device may have no music subunit>")
            }
        }

        // MARK: Family sections
        let emitter = Emitter(line: { line($0) }, head: { head($0) })
        if let bridgeCo = s.bridgeCo {
            BridgeCoReportSection.render(bridgeCo, into: emitter)
        }

        // MARK: Notes
        if !s.notes.isEmpty {
            head("NOTES")
            for note in s.notes { line("  - \(note)") }
        }

        return o
    }
}
