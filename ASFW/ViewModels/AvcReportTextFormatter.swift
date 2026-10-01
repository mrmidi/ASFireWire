import Foundation

enum AvcReportTextFormatter {
    static func format(_ report: AvcReportSnapshot) -> String {
        var lines = ["ASFW AV/C Device Report", "Report schema: \(report.schemaVersion)",
                     "Captured: \(report.capturedAt.formatted(.iso8601))",
                     "Report app: \(report.appVersion)",
                     "Driver: \(report.driverVersion ?? "<unavailable>")",
                     "Scope: Refresh reruns each device's own AV/C bring-up; the exchange log is every FCP command the driver sent and the reply.",
                     "Capture time is the export time, not the original discovery time.",
                     "Missing data means unavailable in the export; it does not prove unsupported hardware.",
                     "Device count: \(report.devices.count)"]
        if report.devices.isEmpty {
            lines.append("\nNo AV/C candidates found in Config ROM or cached AV/C discovery.")
        }
        for device in report.devices {
            lines += ["", "========== \(device.vendorName) \(device.modelName) ==========" ,
                      String(format: "GUID: 0x%016llX  node: %u  generation: %u", device.guid, device.nodeID, device.generation),
                      String(format: "Vendor: 0x%06X  model: 0x%06X  state: %@", device.vendorID, device.modelID, device.state),
                      "", "CONFIG ROM UNITS"]
            if device.romUnits.isEmpty { lines.append("  <none reported>") }
            for unit in device.romUnits {
                lines.append(String(format: "  offset=%u specifier=0x%06X version=0x%06X", unit.offset, unit.specifierID, unit.version))
            }
            if let unit = device.avcUnit {
                lines += ["", "AV/C UNIT PLUGS", "  ISO inputs (playback): \(unit.isoInputPlugs); ISO outputs (capture): \(unit.isoOutputPlugs)",
                          "  External inputs: \(unit.externalInputPlugs); external outputs: \(unit.externalOutputPlugs)",
                          "  Subunits: \(unit.subunits.count)"]
                for subunit in unit.subunits {
                    lines += ["", String(format: "SUBUNIT %@ type=0x%02X id=%u sources=%u destinations=%u", typeName(subunit.type), subunit.type, subunit.id, subunit.sourcePlugs, subunit.destinationPlugs),
                              subunit.capabilitySummary ?? "  Decoded capabilities: <unavailable in discovery export>"]
                }
            } else {
                lines += ["", "AV/C UNIT: <unavailable in discovery export>"]
            }
            if let log = device.exchanges {
                lines += ["", "FCP EXCHANGES", "  " + exchangeSummary(log)]
            } else {
                lines += ["", "FCP EXCHANGES: <unavailable in discovery export>"]
            }
            if !device.notes.isEmpty { lines += ["", "NOTES"] + device.notes.map { "- \($0)" } }
        }

        // Keep all interpretation and inventory together before listing any binary evidence.
        lines += ["", "========== RAW BINARY APPENDIX =========="]
        var rawIndex = 0
        for device in report.devices {
            let guid = String(format: "%016llX", device.guid)
            rawIndex += 1
            appendBytes(device.configROM, title: "RAW \(rawIndex): CONFIG ROM (GUID \(guid))", into: &lines)
            if let log = device.exchanges {
                rawIndex += 1
                appendExchanges(log, title: "RAW \(rawIndex): FCP EXCHANGES (GUID \(guid), session \(log.session))", into: &lines)
            }
            for subunit in device.avcUnit?.subunits ?? [] {
                rawIndex += 1
                appendBytes(subunit.capabilities, title: String(format: "RAW %u: SUBUNIT CAPABILITIES (GUID %@, type 0x%02X, id %u; ASFW user-client serialization, not an AV/C reply)", rawIndex, guid, subunit.type, subunit.id), into: &lines)
                rawIndex += 1
                let name = subunit.type == 0x0C ? "MUSIC STATUS DESCRIPTOR" : "AUDIO IDENTIFIER DESCRIPTOR"
                appendBytes(subunit.descriptor, title: "RAW \(rawIndex): \(name) (GUID \(guid), type \(String(format: "0x%02X", subunit.type)), id \(subunit.id))", into: &lines)
            }
        }
        if rawIndex == 0 { lines.append("\nNo raw binary blobs were available.") }
        return lines.joined(separator: "\n") + "\n"
    }

    static func capabilitiesSummary(_ caps: AVCMusicCapabilities) -> String {
        let rateName = caps.currentRate == 0xFF ? "unavailable" : AVCMusicCapabilities.SupportedFormat.sampleRateName(for: caps.currentRate)
        let rates = [UInt8(0), 1, 2, 3, 4, 5, 6, 7, 10].filter {
            caps.supportedRatesMask & (UInt32(1) << $0) != 0
        }.map { AVCMusicCapabilities.SupportedFormat.sampleRateName(for: $0) }
        var lines = ["  Audio: \(caps.hasAudioCapability); MIDI: \(caps.hasMidiCapability); SMPTE: \(caps.hasSmpteCapability)",
                     String(format: "  Current rate: %@ (code 0x%02X); supported rate mask: 0x%08X", rateName, caps.currentRate, caps.supportedRatesMask),
                     "  Supported rates: \(rates.isEmpty ? "unavailable" : rates.joined(separator: ", "))",
                     "  Audio ports: in=\(caps.audioInputPorts) out=\(caps.audioOutputPorts); MIDI ports: in=\(caps.midiInputPorts) out=\(caps.midiOutputPorts)",
                     "  SMPTE ports: in=\(caps.smpteInputPorts) out=\(caps.smpteOutputPorts)",
                     "  Plug inventory below does not establish the selected host stream route."]
        for plug in caps.plugs {
            lines.append("  Plug \(plug.plugID) \(plug.isInput ? "destination (subunit input)" : "source (subunit output)") \(plug.typeName): \(plug.name)")
            for block in plug.signalBlocks {
                lines.append("    \(block.formatCodeName): \(block.channelCount) channels")
                for channel in block.channels {
                    lines.append("      musicPlug=\(channel.musicPlugID) slot=\(channel.position) name=\(channel.name)")
                }
            }
            for format in plug.supportedFormats {
                lines.append("    Supported: \(format.sampleRateName), \(format.formatCodeName), \(format.channelCount) channels")
            }
        }
        return lines.joined(separator: "\n")
    }

    /// AV/C response codes (the first byte of a reply).
    static func responseName(_ code: UInt8) -> String {
        switch code {
        case 0x08: "NOT IMPLEMENTED"
        case 0x09: "ACCEPTED"
        case 0x0A: "REJECTED"
        case 0x0B: "IN TRANSITION"
        case 0x0C: "STABLE"
        case 0x0D: "CHANGED"
        case 0x0F: "INTERIM"
        default: String(format: "code 0x%02X", code)
        }
    }

    /// What an exchange came to. A refusal of a format-list query at an index
    /// is how a device says the list ended, not an error.
    static func exchangeResult(_ record: AvcReportSnapshot.Exchange) -> String {
        guard record.outcome == "response" else { return record.outcome }
        guard let code = record.response.first else { return "empty reply" }
        let command = record.command
        let isListQuery = command.count > 3 && (command[2] == 0x2F || command[2] == 0xBF) && command[3] == 0xC1
        if isListQuery && (code == 0x0A || code == 0x08) { return "end of list" }
        return responseName(code)
    }

    static func exchangeSummary(_ log: AvcReportSnapshot.ExchangeLog) -> String {
        var counts: [String: Int] = [:]
        for record in log.records {
            counts[exchangeResult(record), default: 0] += 1
        }
        let parts = counts.sorted { $0.value == $1.value ? $0.key < $1.key : $0.value > $1.value }
            .map { "\($0.key) \($0.value)" }
        return "\(log.records.count) exchanges in session \(log.session)" +
            (parts.isEmpty ? "" : ": " + parts.joined(separator: ", ")) +
            (log.dropped > 0 ? "; \(log.dropped) not kept (log full)" : "")
    }

    private static func appendExchanges(_ log: AvcReportSnapshot.ExchangeLog, title: String, into lines: inout [String]) {
        lines += ["", title, "  " + exchangeSummary(log)]
        let hex: ([UInt8]) -> String = { $0.map { String(format: "%02X", $0) }.joined(separator: " ") }
        for record in log.records {
            let result = exchangeResult(record)
            var flags = ""
            if record.interim { flags += " after INTERIM" }
            if record.retries > 0 { flags += " after \(record.retries) retries" }
            let code = record.response.first.map { String(format: " (%@)", responseName($0)) } ?? ""
            let shown = result == "end of list" ? result + code : result
            lines.append(String(format: "  #%04u g%u %@%@", record.sequence, record.generation, shown, flags))
            lines.append("    > " + hex(record.command))
            if !record.response.isEmpty { lines.append("    < " + hex(record.response)) }
        }
    }

    private static func typeName(_ type: UInt8) -> String {
        switch type {
        case 0x01: "Audio"
        case 0x0C: "Music"
        case 0x04: "Tape"
        case 0x09: "Panel"
        default: "Other"
        }
    }

    private static func appendBytes(_ data: Data?, title: String, into lines: inout [String]) {
        guard let data else {
            lines += ["", title, "  <unavailable; export limit is 4096 bytes per blob>"]
            return
        }
        lines += ["", title, "  \(data.count) bytes\(data.isEmpty ? " (empty)" : "")"]
        let bytes = Array(data)
        for offset in stride(from: 0, to: bytes.count, by: 16) {
            let hex = bytes[offset..<min(offset + 16, bytes.count)].map { String(format: "%02X", $0) }.joined(separator: " ")
            lines.append(String(format: "  %04X  %@", offset, hex))
        }
    }
}
