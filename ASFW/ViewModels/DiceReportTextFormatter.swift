//
//  DiceReportTextFormatter.swift
//  ASFW
//
//  Renders a DiceSnapshot as a plain-text report suitable for pasting into a
//  bug report. The goal is that a device ASFW does not support yet can still be
//  fully described by its owner.
//

import Foundation

enum DiceReportTextFormatter {

    /// String(format:) does not honour width specifiers for %@ on macOS, so
    /// columns are padded explicitly.
    private static func pad(_ text: String, _ width: Int) -> String {
        text.count >= width ? text
            : text + String(repeating: " ", count: width - text.count)
    }

    static func format(snapshot s: DiceSnapshot, appVersion: String?) -> String {
        var o = ""
        func line(_ t: String = "") { o += t + "\n" }
        func head(_ t: String) {
            line()
            line(t)
            line(String(repeating: "-", count: max(8, t.count)))
        }

        line("ASFW DICE DEVICE REPORT")
        line("=======================")
        line("Generated: \(ISO8601DateFormatter().string(from: Date()))")
        if let v = appVersion { line("ASFW:      \(v)") }
        if let mode = s.activeRateMode { line("Rate mode: \(mode.label)") }
        line()
        line("This is a read-only dump of the device's DICE register spaces.")
        line("Paste it whole into the issue; do not trim sections.")

        // MARK: Identity
        head("IDENTITY")
        line(String(format: "GUID:          0x%016llX", s.guid))
        line("Vendor:        \(s.vendorName)")
        line("Model:         \(s.modelName)")
        line("Node / gen:    \(s.nodeId) / \(s.generation)")
        line(String(format: "TCAT vendor:   0x%06X", s.tcatVendorId))
        line(String(format: "TCAT category: 0x%02X%@", s.tcatCategory,
                    s.tcatCategory == 0x04 ? "  (standard DICE)"
                                           : "  (non-standard — identifies the device family)"))
        line(String(format: "TCAT product:  0x%03X", s.tcatProductId))
        line(String(format: "TCAT serial:   %d", s.tcatSerial))
        if let caps = s.eapCaps { line("ASIC:          \(caps.asicLabel)") }

        // MARK: Section tables
        head("SECTION TABLES  (offsets and sizes in quadlets)")
        line("general space @ 0xFFFFE0000000")
        if let t = s.generalSections {
            for (n, sec) in zip(t.names, t.sections) {
                line("  " + pad(n, 15)
                     + String(format: "offset=0x%05X (0x%06X B)  size=0x%05X (%d B)",
                              sec.offsetQuadlets, sec.byteOffset,
                              sec.sizeQuadlets, sec.byteSize))
            }
        } else {
            line("  <unreadable>")
        }
        line()
        line("extension space @ 0xFFFFE0200000")
        if let t = s.extensionSections {
            for (n, sec) in zip(t.names, t.sections) {
                line("  " + pad(n, 15)
                     + String(format: "offset=0x%05X (0x%06X B)  size=0x%05X (%d B)",
                              sec.offsetQuadlets, sec.byteOffset,
                              sec.sizeQuadlets, sec.byteSize))
            }
        } else {
            line("  <absent>")
        }

        // MARK: Global
        head("GLOBAL")
        if let g = s.global {
            line(String(format: "OWNER              = 0x%016llX%@", g.ownerRaw,
                        g.hasOwner ? String(format: "  node=0x%04X", g.ownerNodeId)
                                   : "  (no owner)"))
            let notes = DiceNotificationBits.names(g.notification)
            line(String(format: "NOTIFICATION       = 0x%08X   %@", g.notification,
                        (notes.isEmpty ? "-" : notes.joined(separator: " ")) as NSString))
            line("NICK_NAME          = '\(g.nickname)'")
            line(String(format: "CLOCK_SELECT       = 0x%08X   source=%d (%@)  rate=%d (%@)",
                        g.clockSelect, g.clockSourceIndex,
                        DiceClockSource.name(g.clockSourceIndex) as NSString,
                        g.clockRateIndex, DiceClockRate.name(g.clockRateIndex) as NSString))
            line(String(format: "ENABLE             = 0x%08X   streaming=%@",
                        g.enable, (g.enable != 0 ? "yes" : "no") as NSString))
            line(String(format: "STATUS             = 0x%08X   locked=%@  nominal=%@",
                        g.status, (g.sourceLocked ? "yes" : "no") as NSString,
                        DiceClockRate.name(g.nominalRateIndex) as NSString))
            let lk = DiceExtStatusBits.locked(g.extendedStatus)
            let sl = DiceExtStatusBits.slipped(g.extendedStatus)
            line(String(format: "EXTENDED_STATUS    = 0x%08X", g.extendedStatus))
            line("    locked  : \(lk.isEmpty ? "-" : lk.joined(separator: " "))")
            line("    slipped : \(sl.isEmpty ? "-" : sl.joined(separator: " "))")
            line("    NOTE: slip bits are read-to-clear and fluctuate without notification;")
            line("          this is an instantaneous sample, not a stable value.")
            line(String(format: "SAMPLE_RATE        = %d Hz  (measured)", g.measuredRate))
            line(String(format: "VERSION            = 0x%08X   (%d.%d.%d.%d)", g.version,
                        (g.version >> 24) & 0xFF, (g.version >> 16) & 0xFF,
                        (g.version >> 8) & 0xFF, g.version & 0xFF))
            line(String(format: "CLOCK_CAPABILITIES = 0x%08X", g.clockCaps))
            line("    rates   : \(DiceClockCaps.rates(g.clockCaps).joined(separator: " "))")
            line("    sources : \(DiceClockCaps.sources(g.clockCaps).joined(separator: " "))")
            line("CLOCK_SOURCE_NAMES:")
            for (i, n) in g.clockSourceNames.enumerated() {
                let spec = DiceClockSource(rawValue: UInt32(i))?.label ?? "?"
                line("    " + pad("\(i)", 4) + pad(spec, 10) + "'\(n)'")
            }
        } else {
            line("<unreadable>")
        }

        // MARK: Streams
        head("TX STREAMS  (device transmits -> host capture)")
        line("NUMBER = \(s.txCount)   SIZE = \(s.txSizeQuadlets) quadlets (\(s.txSizeQuadlets * 4) bytes)")
        for e in s.txStreams { emitStream(e, isTx: true, into: &o) }

        head("RX STREAMS  (device receives <- host playback)")
        line("NUMBER = \(s.rxCount)   SIZE = \(s.rxSizeQuadlets) quadlets (\(s.rxSizeQuadlets * 4) bytes)")
        for e in s.rxStreams { emitStream(e, isTx: false, into: &o) }

        // MARK: Ext sync
        head("EXT_SYNC")
        if let x = s.extSync {
            line("CLOCK_SOURCE   = \(x.clockSource) (\(DiceClockSource.name(x.clockSource)))")
            line("LOCKED         = \(x.locked ? "yes" : "no")")
            line("RATE           = \(x.rateIndex) (\(DiceClockRate.name(x.rateIndex)))")
            line("ADAT_USER_DATA = " + (x.adatUserData.map { String(format: "0x%X", $0) } ?? "no-data"))
        } else {
            line("<absent>")
        }

        // MARK: EAP
        head("EAP CAPABILITIES")
        if let c = s.eapCaps {
            line("Router : exposed=\(c.routerExposed) readOnly=\(c.routerReadOnly) storable=\(c.routerStorable) maxEntries=\(c.routerMaxEntries)")
            line("Mixer  : exposed=\(c.mixerExposed) readOnly=\(c.mixerReadOnly) storable=\(c.mixerStorable) inDevId=\(c.mixerInputDeviceId) outDevId=\(c.mixerOutputDeviceId) inputs=\(c.mixerInputCount) outputs=\(c.mixerOutputCount)")
            line("General: dynamicStreamFormat=\(c.dynamicStreamFormat) storage=\(c.storageAvailable) peak=\(c.peakAvailable)")
            line("         maxTxStreams=\(c.maxTxStreams) maxRxStreams=\(c.maxRxStreams) formatStorable=\(c.streamFormatStorable)")
            line("         asic=\(c.asicLabel)")
        } else {
            line("<absent>")
        }

        // The router and stream_format sections are staging areas: software
        // writes them and then issues a LOAD opcode through the cmd section to
        // make them active (cmd_section.rs:75-79). What the device is actually
        // running lives in current_config. Empty here is normal.
        head("EAP STREAM FORMAT STAGING AREA  (written, then LOADed — not the live config)")
        if let f = s.eapStreamFormats, f.txEntries.isEmpty == false || f.rxEntries.isEmpty == false {
            emitFormats(f, into: &o)
        } else {
            line("  <empty — nothing staged>")
        }

        if !s.eapCurrentFormats.isEmpty {
            head("EAP CURRENT CONFIG — STREAM FORMATS PER RATE MODE")
            line("These describe the layout at EVERY rate mode. The plain TX/RX")
            line("registers above only describe the mode the device is in now.")
            for mode in DiceRateMode.allCases {
                guard let f = s.eapCurrentFormats[mode] else { continue }
                line()
                line("[\(mode.label)]")
                emitFormats(f, into: &o)
            }
        }

        if let st = s.eapStandalone {
            head("EAP STANDALONE  (behaviour with no host attached)")
            line("clockSource     = \(st.clockSource) (\(DiceClockSource.name(st.clockSource)))")
            line("aesHighRate     = \(st.aesHighRate)")
            line("adatMode        = \(st.adatModeLabel)")
            line("wordClockMode   = \(st.wordClockModeLabel)  rate=\(st.wordClockNumerator)/\(st.wordClockDenominator)")
            line("internalRate    = \(DiceClockRate.name(st.internalRateIndex))")
        }

        head("EAP ROUTER STAGING AREA  (written, then LOADed — not the live config)")
        if s.eapRouter.isEmpty {
            line("  <empty — nothing staged>")
        } else {
            emitRouter(s.eapRouter, showPeak: false, into: &o)
        }

        // Print every rate mode, including empty ones: for an unknown device
        // "read and empty" and "not read" must not look the same.
        for mode in DiceRateMode.allCases {
            let r = s.eapCurrentRouters[mode]
            let active = s.activeRateMode == mode ? "  [ACTIVE]" : ""
            head("EAP CURRENT ROUTER — \(mode.label)  (\(r?.count ?? 0) entries)\(active)")
            if let r, r.isEmpty == false {
                emitRouter(r, showPeak: false, into: &o)
            } else if r == nil {
                line("  <not read>")
            } else {
                line("  <empty — this device advertises no rates in this mode>")
            }
        }

        if s.eapPeak.isEmpty == false {
            // The section is sized by routerMaxEntries; only the entries that
            // mirror a live route carry data. The rest is uninitialised memory
            // and would read as plausible-looking nonsense.
            let n = s.meaningfulPeakCount
            head("EAP PEAK  (\(n) of \(s.eapPeak.count) entries carry data, instantaneous)")
            // tcd22xx_ctl.rs:1195-1196 -- "Upper 12 bits of each sample".
            line("Peak is 12-bit: full scale = 4095.  dBFS = 20*log10(peak/4095).")
            line("Reading it as 16-bit would understate every level by 24 dB.")
            emitRouter(Array(s.eapPeak.prefix(n)), showPeak: true, into: &o)
            if n < s.eapPeak.count {
                line("  (\(s.eapPeak.count - n) further slots are beyond the active router "
                     + "and hold uninitialised data — omitted)")
            }
        }

        if let m = s.eapMixer, !m.coefficients.isEmpty {
            head("EAP MIXER  (\(m.coefficients.count) outputs x \(m.coefficients[0].count) inputs)")
            // tcd22xx_ctl.rs:700-702 -- range 0..0xFFFF, "2:14 Fixed-point".
            line("Coefficients are 2:14 fixed point: gain = value/16384.")
            line("  0 = muted,  16384 (0x4000) = unity (0 dB),  0xFFFF = +12.04 dB")
            line("  dB = 20*log10(value/16384)")
            line(String(format: "saturation = 0x%08X   (bit n set = output n clipped)",
                        m.saturation))
            var header = "      "
            for i in 0..<m.coefficients[0].count {
                header += String(format: "%6d", i)
            }
            line(header)
            for (o0, row) in m.coefficients.enumerated() {
                var r = String(format: "  %3d ", o0)
                for v in row { r += String(format: "%6d", v) }
                line(r)
            }
        }

        if !s.notes.isEmpty {
            head("NOTES")
            for n in s.notes { line("- \(n)") }
        }

        return o
    }

    // MARK: - Helpers

    private static func emitStream(_ e: DiceStreamEntry, isTx: Bool, into o: inout String) {
        func line(_ t: String = "") { o += t + "\n" }
        line()
        line("  [stream \(e.index)]")
        line("    ISOCHRONOUS  = \(e.isoChannel)\(e.isoChannel < 0 ? "  (disabled)" : "")")
        if let seq = e.sequenceStart { line("    SEQ_START    = \(seq)") }
        line("    PCM channels = \(e.pcmChannels)")
        line("    MIDI ports   = \(e.midiPorts)")
        if e.speed != nil { line("    SPEED        = \(e.speedLabel)") }
        if let caps = e.ac3Capabilities, let en = e.ac3Enable {
            line(String(format: "    AC3_CAPS     = 0x%08X", caps))
            line(String(format: "    AC3_ENABLE   = 0x%08X", en))
        } else {
            line("    AC3_CAPS     = <not implemented: stream block too short>")
        }
        line("    NAMES:")
        for (i, n) in e.names.enumerated() {
            line("      " + pad("\(i)", 4) + "'\(n)'")
        }
    }

    private static func emitFormats(_ f: DiceEapStreamFormats, into o: inout String) {
        func line(_ t: String = "") { o += t + "\n" }
        func emit(_ label: String, _ entries: [DiceEapFormatEntry]) {
            for (i, e) in entries.enumerated() {
                line("  \(label) \(i): pcm=\(e.pcmChannels) midi=\(e.midiPorts)"
                     + String(format: "  ac3=0x%08X", e.ac3Mask))
                if !e.names.isEmpty {
                    line("    names: \(e.names.joined(separator: ", "))")
                }
            }
        }
        emit("tx", f.txEntries)
        emit("rx", f.rxEntries)
        if f.txEntries.isEmpty && f.rxEntries.isEmpty { line("  <no entries>") }
    }

    private static func emitRouter(_ entries: [DiceRouterEntry], showPeak: Bool,
                                   into o: inout String) {
        func line(_ t: String = "") { o += t + "\n" }
        for (i, e) in entries.enumerated() {
            let head = "  " + pad("\(i)", 5) + pad(e.dstLabel, 18) + "<- "
            if showPeak {
                line(head + pad(e.srcLabel, 18) + String(format: "peak=%5d", e.peak))
            } else {
                line(head + e.srcLabel)
            }
        }
    }
}
