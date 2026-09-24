// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// BeBoBShellTelemetry.swift — parsers for the BridgeCo Virtual UART diagnostic
// shell's textual output (`sys stat`, `sys avstat`, `fw show`).
//
// These are pure functions over captured stdout so they can be tested against
// real device output. Fixtures live in ASFWTests/BeBoBShellFixtures.swift and
// are verbatim captures from an M-Audio FW 1814 — not hand-written samples.
//
// The `sys stat` layout is confirmed against the firmware's own format strings
// (fw1814.elf: "rxIsr %9d  rxStreamInvalid %5d  MDBAliveErrors %6d" for the
// global block, "%c %12s" / "%14s" / "%7d %%" for the stream tables).

import Foundation

// MARK: - sys stat

/// Which side of the chip a stream terminates on. The `dest` (output) and
/// `source` (input) rows of `sys stat` carry this, and it is the ONLY reliable
/// way to find the FireWire-facing stream: column order is not fixed, and on
/// the 1814 the host stream is column 1 of the output table but column 0 of the
/// input table. A parser that takes the first value on each row reports the
/// device's internal S/PDIF-ADAT path as if it were ours.
public enum BeBoBStreamEndpoint: String, Equatable, Sendable {
    case fireWire = "1394"
    case av = "AV"
    case unknown = "?"
}

/// One column of a `sys stat` stream table: a single isochronous stream.
public struct BeBoBStreamColumn: Equatable, Sendable {
    public var isoChannel: Int = -1
    public var endpoint: BeBoBStreamEndpoint = .unknown
    public var speed: Int = 0

    /// Counter rows keyed by the device's own label, verbatim ("txWrite",
    /// "pkt Future", "SytOffset", …). Unrecognised labels are kept rather than
    /// dropped: the label set is the firmware's, not ours.
    public var counters: [String: Int64] = [:]

    /// Severity character the firmware printed for a row: "W", "E" or "S".
    /// Rows with no prefix are informational.
    public var severity: [String: String] = [:]

    public subscript(_ label: String) -> Int64? { counters[label] }

    public func value(_ label: String, default fallback: Int64 = 0) -> Int64 {
        counters[label] ?? fallback
    }

    /// Labels the firmware prints with a trailing `%` (`"%7d %%"`).
    public static let percentLabels: Set<String> = [
        "txQFillLevel", "rxQFillLevel", "PoolFillLevel"
    ]
}

public struct BeBoBStreamingStats: Equatable, Sendable {
    /// `global statistic:` — three label/value pairs per line, not columns.
    public var global: [String: Int64] = [:]

    /// `Output Stream statistic:` columns, in printed order.
    public var outputs: [BeBoBStreamColumn] = []

    /// `Input Stream statistic:` columns, in printed order.
    public var inputs: [BeBoBStreamColumn] = []

    /// True when the device printed "No active output streams."
    public var noActiveOutputStreams: Bool = false

    /// The output stream that leaves over FireWire — the one ASFW receives.
    public var fireWireOutput: BeBoBStreamColumn? {
        outputs.first { $0.endpoint == .fireWire }
    }

    /// The input stream that arrives over FireWire — the one ASFW transmits.
    public var fireWireInput: BeBoBStreamColumn? {
        inputs.first { $0.endpoint == .fireWire }
    }

    public var isEmpty: Bool {
        global.isEmpty && outputs.isEmpty && inputs.isEmpty
    }
}

// MARK: - sys avstat

/// One silicon latch: `sys avstat` prints EVERY latch unconditionally, as
/// "    SetTgInLock       : 00000001", so the presence of a label carries no
/// information — only its value does. Values are hex without an `0x` prefix.
public struct BeBoBSiliconLatch: Equatable, Sendable {
    public var block: String
    public var label: String
    public var value: UInt32

    public var isSet: Bool { value != 0 }
}

public struct BeBoBAvStat: Equatable, Sendable {
    /// Latches in printed order, tagged with the block they appeared under
    /// (TGEN, AV1, AV2, FRAMER/DEFRAMER, MDB, LLC). Block scoping matters:
    /// `SetTSErr`, `SyncWarn`, `DataErr` and `IntMask` all appear in both AV1
    /// and AV2, so a document-wide search for them is ambiguous.
    public var latches: [BeBoBSiliconLatch] = []

    public func latch(_ label: String, block: String? = nil) -> BeBoBSiliconLatch? {
        latches.first { $0.label == label && (block == nil || $0.block == block) }
    }

    public func isSet(_ label: String, block: String? = nil) -> Bool {
        latch(label, block: block)?.isSet ?? false
    }

    /// Every latch currently reading non-zero, for "what is actually wrong".
    public var setLatches: [BeBoBSiliconLatch] { latches.filter(\.isSet) }

    public var setTgInLock: Bool { isSet("SetTgInLock", block: "TGEN") }
    public var setTgSytMiss: Bool { isSet("SetTgSytMiss", block: "TGEN") }
    public var cipMismatch: Bool { isSet("CIPMismatch") }
    public var dbcMismatch: Bool { isSet("DBCMismatch") }
    public var fmtMismatch: Bool { isSet("FMTMismatch") }
    public var sidMismatch: Bool { isSet("SIDMismatch") }
    public var headerMismatch: Bool { isSet("HeaderMismatch") }

    public var isEmpty: Bool { latches.isEmpty }
}

// MARK: - fw show

public struct BeBoBSyncState: Equatable, Sendable {
    public var audioState: String = "Unknown"
    public var syncSource: String = "Unknown"
    public var sampleRateHz: UInt32 = 0

    /// `Input Source` / `Output Source` — SPDIF or ADAT. These select which
    /// half of the hardcoded channel formation table is correct; ASFW asserts
    /// SPDIF without asking, so a device booted in ADAT would be framed wrong.
    public var inputSource: String = "Unknown"
    public var outputSource: String = "Unknown"
    public var spdifSource: String = "Unknown"

    /// Iso channel ASSIGNMENTS, not channel counts. `-1` means unassigned,
    /// which is a legal and common value (MIDI and AC3 are usually -1).
    public var isoChannels: [String: Int] = [:]

    public var swReturnChannel: Int? { isoChannels["SwReturn"] }
    public var swSendChannel: Int? { isoChannels["SwSend"] }
}

// MARK: - Parser

public enum BeBoBShellTelemetryParser {

    // MARK: sys stat

    private enum Section { case none, global, output, input }

    public static func parseStreamingStats(_ text: String) -> BeBoBStreamingStats? {
        guard !text.isEmpty else { return nil }
        var stats = BeBoBStreamingStats()
        var section = Section.none
        var columns: [BeBoBStreamColumn] = []

        func commit() {
            switch section {
            case .output: stats.outputs = columns
            case .input: stats.inputs = columns
            case .global, .none: break
            }
            columns = []
        }

        for rawLine in text.components(separatedBy: .newlines) {
            let line = rawLine.trimmingCharacters(in: .whitespaces)
            if line.isEmpty { continue }

            if line.hasPrefix("global statistic:") {
                commit(); section = .global; continue
            }
            if line.hasPrefix("Output Stream statistic:") {
                commit(); section = .output; continue
            }
            if line.hasPrefix("Input Stream statistic:") {
                commit(); section = .input; continue
            }
            if line.hasPrefix("No active output streams.") {
                stats.noActiveOutputStreams = true; continue
            }
            // The shell echoes the command and prints its prompt; neither is data.
            if line.hasSuffix("sys stat") || line.hasSuffix(">") { continue }

            switch section {
            case .none:
                continue

            case .global:
                for (label, value) in parseGlobalPairs(line) {
                    stats.global[label] = value
                }

            case .output, .input:
                guard let row = parseStreamRow(line) else { continue }
                while columns.count < row.values.count {
                    columns.append(BeBoBStreamColumn())
                }
                for (index, token) in row.values.enumerated() where index < columns.count {
                    apply(token: token, label: row.label, severity: row.severity,
                          to: &columns[index])
                }
            }
        }
        commit()
        return stats.isEmpty ? nil : stats
    }

    /// `rxIsr  9691179   rxStreamInvalid  0   MDBAliveErrors  0` — repeated
    /// label/value pairs on one line, per the firmware's format strings.
    private static func parseGlobalPairs(_ line: String) -> [(String, Int64)] {
        let tokens = line.split(separator: " ", omittingEmptySubsequences: true).map(String.init)
        var pairs: [(String, Int64)] = []
        var index = 0
        while index + 1 < tokens.count {
            let label = tokens[index]
            if let value = Int64(tokens[index + 1]), !isNumeric(label) {
                pairs.append((label, value))
                index += 2
            } else {
                index += 1
            }
        }
        return pairs
    }

    private struct StreamRow {
        var severity: String?
        var label: String
        var values: [String]
    }

    /// `E     pkt Future        4        0        0` →
    /// severity "E", label "pkt Future", values ["4", "0", "0"].
    ///
    /// The label may contain spaces ("pkt Future", "iso channel") and the value
    /// run may be non-numeric ("AV", "1394") or carry `%` markers, so the split
    /// point is the first value-like token rather than a fixed column.
    private static func parseStreamRow(_ line: String) -> StreamRow? {
        var tokens = line.split(separator: " ", omittingEmptySubsequences: true).map(String.init)
        guard !tokens.isEmpty else { return nil }

        var severity: String?
        if tokens.count > 1, ["W", "E", "S"].contains(tokens[0]) {
            severity = tokens.removeFirst()
        }

        guard let split = tokens.firstIndex(where: isValueToken), split > 0 else { return nil }
        let label = tokens[..<split].joined(separator: " ")
        let values = tokens[split...].filter { $0 != "%" }
        guard !values.isEmpty else { return nil }
        return StreamRow(severity: severity, label: label, values: values)
    }

    private static func isValueToken(_ token: String) -> Bool {
        if token == BeBoBStreamEndpoint.av.rawValue { return true }
        return isNumeric(token)
    }

    private static func isNumeric(_ token: String) -> Bool {
        var body = Substring(token)
        if body.first == "-" { body = body.dropFirst() }
        return !body.isEmpty && body.allSatisfy(\.isNumber)
    }

    private static func apply(token: String, label: String, severity: String?,
                              to column: inout BeBoBStreamColumn) {
        switch label {
        case "iso channel":
            column.isoChannel = Int(token) ?? -1
        case "dest", "source":
            column.endpoint = BeBoBStreamEndpoint(rawValue: token) ?? .unknown
        case "speed":
            column.speed = Int(token) ?? 0
        default:
            guard let value = Int64(token) else { return }
            column.counters[label] = value
            if let severity { column.severity[label] = severity }
        }
    }

    // MARK: sys avstat

    public static func parseAvStat(_ text: String) -> BeBoBAvStat? {
        guard !text.isEmpty else { return nil }
        var stat = BeBoBAvStat()
        var block = ""

        for rawLine in text.components(separatedBy: .newlines) {
            let line = rawLine.trimmingCharacters(in: .whitespaces)
            guard let colon = line.firstIndex(of: ":") else { continue }

            let label = line[..<colon].trimmingCharacters(in: .whitespaces)
            let rest = line[line.index(after: colon)...].trimmingCharacters(in: .whitespaces)
            guard !label.isEmpty else { continue }

            // A block header names a peripheral base address and is not indented:
            // "TGEN                  : FE700000". Register rows carry their own
            // address in parentheses; latch rows are indented under the block.
            let indented = rawLine.hasPrefix(" ") || rawLine.hasPrefix("\t")
            if !indented, !label.contains("("), rest.count == 8, UInt32(rest, radix: 16) != nil,
               rest.hasPrefix("FE") {
                block = label
                continue
            }

            // Multi-value rows ("00000001, 00000000, 00000000") take the first.
            let first = rest.split(separator: ",").first.map {
                $0.trimmingCharacters(in: .whitespaces)
            } ?? rest
            guard let value = UInt32(first, radix: 16) else { continue }

            stat.latches.append(BeBoBSiliconLatch(block: block, label: label, value: value))
        }
        return stat.isEmpty ? nil : stat
    }

    // MARK: fw show

    public static func parseSyncState(_ text: String) -> BeBoBSyncState? {
        guard !text.isEmpty else { return nil }
        var sync = BeBoBSyncState()
        var sawAnything = false

        for rawLine in text.components(separatedBy: .newlines) {
            let line = rawLine.trimmingCharacters(in: .whitespaces)
            guard let equals = line.firstIndex(of: "=") else { continue }
            var label = String(line[..<equals]).trimmingCharacters(in: .whitespaces)
            let value = line[line.index(after: equals)...].trimmingCharacters(in: .whitespaces)
            // Iso channel rows are dot-padded: "LineIn .....     = 62".
            while label.hasSuffix(".") || label.hasSuffix(" ") {
                label = String(label.dropLast())
            }
            guard !label.isEmpty, !value.isEmpty else { continue }
            sawAnything = true

            switch label {
            case "Sampling Frequency":
                sync.sampleRateHz = parseRate(value)
            case "Sync Source":
                // `fw show` prints this twice: a long form and a short code
                // ("Internal Digital Input Sync" then "INTDIG"). Keep the long
                // one, which is the descriptive of the two.
                if sync.syncSource == "Unknown" || value.contains(" ") {
                    sync.syncSource = value
                }
            case "Audio State":
                sync.audioState = value
            case "Input Source":
                sync.inputSource = value
            case "Output Source":
                sync.outputSource = value
            case "Spdif Source":
                sync.spdifSource = value
            case "Timer Action":
                continue
            default:
                if let channel = Int(value) {
                    sync.isoChannels[label] = channel
                }
            }
        }
        return sawAnything ? sync : nil
    }

    private static func parseRate(_ text: String) -> UInt32 {
        let digits = text.replacingOccurrences(of: "kHz", with: "")
            .trimmingCharacters(in: .whitespaces)
        guard let value = Double(digits) else { return 0 }
        return UInt32((value * 1000).rounded())
    }
}
