import Foundation

// Live read-only telemetry tool surface: controller state, topology, Config ROM.
//
// These three tools were previously advertised but dispatched to
// `notImplementedToolResult`, so an agent following the documented
// "prefer a structured MCP read for current state" workflow got
// `capabilityUnavailable` from the three most obvious state queries while the
// equivalent *resources* returned real data. The data path already existed —
// only the tool arms were unrouted.
//
// Nothing here introduces a new driver capability: topology comes from the
// diagnostics ABI (`ASFWDiagTopology`, selector 1001) that the Topology view
// already consumes, and Config ROM comes from the existing cached-ROM export
// plus the app's `RomParser`.

// MARK: - Topology models

struct ASFWMCPTopologyPort: Equatable {
    let port: Int
    /// Reported Self-ID port state. The raw 2-bit encoding is the wire contract
    /// (NotPresent=0, NotActive=1, Parent=2, Child=3) and is mapped by case, never
    /// by arithmetic on the raw value.
    let state: String
    let remoteNodeId: Int?
    let remotePort: Int?
}

struct ASFWMCPTopologyNode: Equatable {
    let nodeId: Int
    let portCount: Int
    let gapCount: Int
    let speedMbps: UInt32
    let linkActive: Bool
    /// Self-ID contender bit. Contender + link-active is what *designates* the IRM;
    /// it is not proof that the node's resource CSRs have been used successfully.
    let contender: Bool
    let isRoot: Bool
    let initiatedReset: Bool
    let parentPort: Int?
    let ports: [ASFWMCPTopologyPort]
}

struct ASFWMCPTopologySnapshot: Equatable {
    let generation: UInt32
    let nodeCount: Int
    let rootNodeId: Int?
    let irmNodeId: Int?
    let localNodeId: Int?
    let gapCount: Int
    let busBase16: UInt32
    let nodes: [ASFWMCPTopologyNode]
    let warnings: [String]
}

// MARK: - Config ROM models

struct ASFWMCPConfigRomUnit: Equatable {
    let specifierId: UInt32?
    let version: UInt32?
    let modelId: UInt32?
    let modelName: String?
}

/// One agent-facing BIB row. Keeping the bit location and a one-sentence
/// explanation beside the value avoids repeated mask/spec research without
/// turning routine Config-ROM reads into a raw-data dump.
struct ASFWMCPConfigRomBIBField: Equatable {
    let name: String
    let bits: String
    let value: String
    let meaning: String
}

/// A loss-conscious projection of one parsed IEEE 1212 directory entry. The
/// full parser stays in the app model; this DTO intentionally omits arbitrary
/// leaf payload bytes because agents normally need the decoded descriptor or
/// the fact that a target was not fetched from the partial cache.
struct ASFWMCPConfigRomTreeEntry: Equatable {
    let path: String
    let key: String
    let keyId: UInt8
    let entryType: String
    let entryQuadlet: Int?
    let relativeOffsetQuadlets: Int?
    let targetQuadlet: Int?
    let value: String?
    let children: [ASFWMCPConfigRomTreeEntry]
}

struct ASFWMCPConfigRomSummary: Equatable {
    let nodeId: UInt32
    let requestedGeneration: UInt32
    let resolvedGeneration: UInt32
    /// False means the driver served a cache entry from a different generation.
    /// Node ids from another generation are not interchangeable with this one.
    let exactGenerationMatch: Bool
    let byteCount: Int
    let quadletCount: Int
    /// The root-directory header follows q0 plus `bus_info_length` BIB
    /// quadlets. It is nil when the cache did not contain a parseable BIB.
    let rootDirectoryStartQuadlet: Int?
    /// False when the cached bytes could not be parsed as a complete ROM. The
    /// discovery cache legitimately holds only a fetched *prefix*, so this is not
    /// by itself a malformed-device-ROM finding.
    let parsed: Bool
    let parseNote: String?
    let guid: String?
    let busName: String?
    let irmc: Bool?
    let cmc: Bool?
    let isc: Bool?
    let bmc: Bool?
    let maxRec: UInt8?
    let linkSpeed: UInt8?
    let vendorName: String?
    let modelName: String?
    let vendorId: UInt32?
    let modelId: UInt32?
    let modalias: String?
    let units: [ASFWMCPConfigRomUnit]
    let diagnostics: [String]
    let bibFields: [ASFWMCPConfigRomBIBField]
    /// Cached bytes in big-endian quadlet order. They are only emitted by the
    /// explicitly requested, bounded raw view.
    let rawQuadlets: [UInt32]
    let tree: [ASFWMCPConfigRomTreeEntry]
}

extension ASFWMCPConfigRomSummary {
    /// Returns the same summary marked unparsed, carrying the reason. Used when the
    /// cached bytes are only a prefix rather than a complete ROM.
    func withParseNote(_ note: String) -> ASFWMCPConfigRomSummary {
        ASFWMCPConfigRomSummary(
            nodeId: nodeId,
            requestedGeneration: requestedGeneration,
            resolvedGeneration: resolvedGeneration,
            exactGenerationMatch: exactGenerationMatch,
            byteCount: byteCount,
            quadletCount: quadletCount,
            rootDirectoryStartQuadlet: rootDirectoryStartQuadlet,
            parsed: false,
            parseNote: note,
            guid: guid,
            busName: busName,
            irmc: irmc, cmc: cmc, isc: isc, bmc: bmc,
            maxRec: maxRec, linkSpeed: linkSpeed,
            vendorName: vendorName, modelName: modelName,
            vendorId: vendorId, modelId: modelId, modalias: modalias,
            units: units,
            diagnostics: diagnostics,
            bibFields: bibFields,
            rawQuadlets: rawQuadlets,
            tree: tree
        )
    }
}

// MARK: - MCP value projections

extension ASFWMCPTopologyPort {
    var mcpValue: ASFWMCPValue {
        .object([
            "port": .int(port),
            "state": .string(state),
            "remoteNodeId": remoteNodeId.map { .int($0) } ?? .null,
            "remotePort": remotePort.map { .int($0) } ?? .null
        ])
    }
}

extension ASFWMCPTopologyNode {
    var mcpValue: ASFWMCPValue {
        .object([
            "nodeId": .int(nodeId),
            "portCount": .int(portCount),
            "gapCount": .int(gapCount),
            "speedMbps": .int(Int(speedMbps)),
            "linkActive": .bool(linkActive),
            "contender": .bool(contender),
            "isRoot": .bool(isRoot),
            "initiatedReset": .bool(initiatedReset),
            "parentPort": parentPort.map { .int($0) } ?? .null,
            "ports": .array(ports.map(\.mcpValue))
        ])
    }
}

extension ASFWMCPTopologySnapshot {
    var mcpValue: ASFWMCPValue {
        .object([
            "generation": .int(Int(generation)),
            "nodeCount": .int(nodeCount),
            "rootNodeId": rootNodeId.map { .int($0) } ?? .null,
            "irmNodeId": irmNodeId.map { .int($0) } ?? .null,
            "localNodeId": localNodeId.map { .int($0) } ?? .null,
            "gapCount": .int(gapCount),
            "busBase16": .int(Int(busBase16)),
            "nodes": .array(nodes.map(\.mcpValue)),
            "warnings": .array(warnings.map { .string($0) })
        ])
    }
}

extension ASFWMCPConfigRomUnit {
    var mcpValue: ASFWMCPValue {
        .object([
            "specifierId": specifierId.map { .int(Int($0)) } ?? .null,
            "version": version.map { .int(Int($0)) } ?? .null,
            "modelId": modelId.map { .int(Int($0)) } ?? .null,
            "modelName": modelName.map { .string($0) } ?? .null
        ])
    }
}

extension ASFWMCPConfigRomBIBField {
    static func makeFields(from bib: BusInfo) -> [ASFWMCPConfigRomBIBField] {
        let options = bib.busOptions
        return [
            .init(name: "bus_info_length", bits: "q0[31:24]", value: "\(bib.header.busInfoLength)", meaning: "BIB quadlets after q0; the root directory starts immediately after them."),
            .init(name: "crc_length", bits: "q0[23:16]", value: "\(bib.header.crcLength)", meaning: "Quadlets covered by the BIB CRC, not the total ROM size."),
            .init(name: "crc", bits: "q0[15:0]", value: String(format: "0x%04X", bib.header.crc), meaning: "16-bit CRC for the BIB coverage region; useful diagnostic evidence."),
            .init(name: "bus_name", bits: "q1", value: String(format: "0x%08X ('%@')", bib.busName, bib.busNameString), meaning: "Bus identifier; normal IEEE 1394 devices report ASCII '1394'."),
            .init(name: "irmc", bits: "q2[31]", value: yesNo(options.irmc), meaning: "Isochronous Resource Manager capable claim."),
            .init(name: "cmc", bits: "q2[30]", value: yesNo(options.cmc), meaning: "Cycle master capable claim."),
            .init(name: "isc", bits: "q2[29]", value: yesNo(options.isc), meaning: "Isochronous capable claim."),
            .init(name: "bmc", bits: "q2[28]", value: yesNo(options.bmc), meaning: "Bus Manager capable claim; ownership requires BUS_MANAGER_ID evidence."),
            .init(name: "pmc", bits: "q2[27]", value: yesNo(options.pmc), meaning: "Power manager capable claim, if the device implements it."),
            .init(name: "cyc_clk_acc", bits: "q2[23:16]", value: String(format: "0x%02X (%d)", options.cycClkAcc, options.cycClkAcc), meaning: "Cycle-clock accuracy code: a timing-quality hint, not a setting."),
            .init(name: "max_rec", bits: "q2[15:12]", value: "\(options.maxRec) (≈ \(1 << Int(options.maxRec + 1)) bytes max async payload)", meaning: "Maximum asynchronous receive payload code; higher values allow larger packets."),
            .init(name: "max_rom", bits: "q2[9:8]", value: "\(options.maxRom)", meaning: "Config-ROM read capability encoding; quadlet reads remain common."),
            .init(name: "generation", bits: "q2[7:4]", value: "\(options.generation)", meaning: "Device BIB generation nibble; it can differ from the host topology generation."),
            .init(name: "link_spd", bits: "q2[2:0]", value: "\(options.linkSpd) (\(linkSpeedLabel(options.linkSpd)))", meaning: "Maximum link-speed code advertised by the device."),
            .init(name: "guid", bits: "q3:q4", value: String(format: "0x%016llX", bib.guid), meaning: "Globally unique node identifier; normally stable across bus resets.")
        ]
    }

    var mcpValue: ASFWMCPValue {
        .object([
            "name": .string(name),
            "bits": .string(bits),
            "value": .string(value),
            "meaning": .string(meaning)
        ])
    }

    private static func yesNo(_ value: Bool) -> String { value ? "Yes" : "No" }

    private static func linkSpeedLabel(_ code: UInt8) -> String {
        switch code {
        case 0: return "S100"
        case 1: return "S200"
        case 2: return "S400"
        case 3: return "S800"
        case 4: return "S1600"
        case 5: return "S3200"
        default: return "Reserved/Unknown"
        }
    }
}

extension ASFWMCPConfigRomTreeEntry {
    var mcpValue: ASFWMCPValue {
        .object([
            "path": .string(path),
            "key": .string(key),
            "keyId": .string(String(format: "0x%02X", keyId)),
            "entryType": .string(entryType),
            "entryQuadlet": entryQuadlet.map { .int($0) } ?? .null,
            "relativeOffsetQuadlets": relativeOffsetQuadlets.map { .int($0) } ?? .null,
            "targetQuadlet": targetQuadlet.map { .int($0) } ?? .null,
            "value": value.map { .string($0) } ?? .null,
            "children": .array(children.map(\.mcpValue))
        ])
    }
}

extension ASFWMCPConfigRomSummary {
    func mcpValue(
        view: ASFWMCPConfigRomView,
        startQuadlet: Int,
        maxQuadlets: Int,
        maxTreeEntries: Int
    ) -> ASFWMCPValue {
        var fields = commonMcpFields()
        fields["view"] = .string(view.rawValue)

        switch view {
        case .summary:
            fields["overview"] = .object([
                "romSize": .string("\(byteCount) bytes (\(quadletCount) quadlets)"),
                "rootDirectoryStart": rootDirectoryStartQuadlet.map { .string("q\($0) (byte \($0 * 4))") } ?? .null,
                "guid": guid.map { .string($0) } ?? .null,
                "cacheNote": .string(cacheNote)
            ])
            fields["identity"] = .object([
                "vendor": identityValue(name: vendorName, id: vendorId),
                "model": identityValue(name: modelName, id: modelId),
                "modalias": modalias.map { .string($0) } ?? .null,
                "unitDirectoryCount": .int(units.count)
            ])
            fields["agentNotes"] = .array(summaryNotes.map { .string($0) })
        case .bib:
            fields["bib"] = .object([
                "purpose": .string("IEEE 1394 self-description header. BIB capability bits are device claims, not topology or Bus Manager ownership evidence."),
                "fields": .array(bibFields.map(\.mcpValue))
            ])
        case .tree:
            var remaining = maxTreeEntries
            let bounded = tree.compactMap { $0.bounded(to: &remaining) }
            fields["tree"] = .object([
                "rootDirectoryStartQuadlet": rootDirectoryStartQuadlet.map { .int($0) } ?? .null,
                "entries": .array(bounded.map(\.mcpValue)),
                "entryLimit": .int(maxTreeEntries),
                "truncated": .bool(remaining == 0 && boundedEntryCount(tree) > maxTreeEntries),
                "note": .string("Text descriptors and immediate values are decoded; raw leaf payload bytes are omitted. A target outside this cache is reported as not fetched, not malformed.")
            ])
        case .raw:
            let start = min(startQuadlet, rawQuadlets.count)
            let end = min(start + maxQuadlets, rawQuadlets.count)
            fields["raw"] = .object([
                "byteOrder": .string("bigEndian"),
                "startQuadlet": .int(start),
                "quadletCount": .int(end - start),
                "availableQuadlets": .int(rawQuadlets.count),
                "truncated": .bool(end < rawQuadlets.count),
                "quadlets": .array(rawQuadlets[start..<end].enumerated().map { offset, word in
                    .object([
                        "index": .int(start + offset),
                        "hex": .string(String(format: "0x%08X", word))
                    ])
                }),
                "note": .string("This is the discovery cache, not a live ROM read. A shorter cache prefix does not prove a target is outside the device ROM.")
            ])
        }

        return .object(fields)
    }

    private func commonMcpFields() -> [String: ASFWMCPValue] {
        [
            "nodeId": .int(Int(nodeId)),
            "requestedGeneration": .int(Int(requestedGeneration)),
            "resolvedGeneration": .int(Int(resolvedGeneration)),
            "exactGenerationMatch": .bool(exactGenerationMatch),
            "byteCount": .int(byteCount),
            "quadletCount": .int(quadletCount),
            "parsed": .bool(parsed),
            "parseNote": parseNote.map { .string($0) } ?? .null,
            "guid": guid.map { .string($0) } ?? .null,
            "busName": busName.map { .string($0) } ?? .null,
            "busOptions": .object([
                "irmc": irmc.map { .bool($0) } ?? .null,
                "cmc": cmc.map { .bool($0) } ?? .null,
                "isc": isc.map { .bool($0) } ?? .null,
                "bmc": bmc.map { .bool($0) } ?? .null,
                "maxRec": maxRec.map { .int(Int($0)) } ?? .null,
                "linkSpeed": linkSpeed.map { .int(Int($0)) } ?? .null
            ]),
            "vendorName": vendorName.map { .string($0) } ?? .null,
            "modelName": modelName.map { .string($0) } ?? .null,
            "units": .array(units.map(\.mcpValue)),
            "diagnostics": .array(diagnostics.map { .string($0) })
        ]
    }

    private var cacheNote: String {
        exactGenerationMatch
            ? "Cached for the requested generation; still treat it as a fetched prefix unless a full read was explicitly performed."
            : "Cache belongs to another generation; node IDs are stale and must not be used for a follow-up transaction."
    }

    private var summaryNotes: [String] {
        var notes = [
            "BIB capability bits (IRMC/BMC/CMC/ISC) are Config-ROM claims. Do not infer physical root, designated IRM, or Bus Manager ownership from them.",
            "The discovery cache may be only a prefix. Missing leaves are not malformed-ROM evidence without a generation-pinned full read."
        ]
        if !exactGenerationMatch {
            notes.insert("Generation mismatch: refresh asfw://nodes before using this node ID in any follow-up request.", at: 0)
        }
        if let parseNote {
            notes.append(parseNote)
        }
        return notes
    }

    private func identityValue(name: String?, id: UInt32?) -> ASFWMCPValue {
        guard let name else {
            return id.map { .string(String(format: "0x%06X", $0)) } ?? .null
        }
        guard let id else { return .string(name) }
        return .string(String(format: "%@ (0x%06X)", name, id))
    }

    private func boundedEntryCount(_ entries: [ASFWMCPConfigRomTreeEntry]) -> Int {
        entries.reduce(0) { $0 + 1 + boundedEntryCount($1.children) }
    }
}

private extension ASFWMCPConfigRomTreeEntry {
    func bounded(to remaining: inout Int) -> ASFWMCPConfigRomTreeEntry? {
        guard remaining > 0 else { return nil }
        remaining -= 1
        let boundedChildren = children.compactMap { $0.bounded(to: &remaining) }
        return ASFWMCPConfigRomTreeEntry(
            path: path,
            key: key,
            keyId: keyId,
            entryType: entryType,
            entryQuadlet: entryQuadlet,
            relativeOffsetQuadlets: relativeOffsetQuadlets,
            targetQuadlet: targetQuadlet,
            value: value,
            children: boundedChildren
        )
    }
}

extension ASFWMCPConfigRomTreeEntry {
    init(entry: DirectoryEntry) {
        self.init(
            path: entry.pathId,
            key: entry.keyName,
            keyId: entry.keyId,
            entryType: String(describing: entry.type),
            entryQuadlet: entry.entryQuadletIndex,
            relativeOffsetQuadlets: entry.relativeOffset24.map { Int($0) },
            targetQuadlet: entry.targetQuadletIndex,
            value: Self.valueDescription(entry.value),
            children: Self.children(entry.value)
        )
    }

    private static func children(_ value: RomValue) -> [ASFWMCPConfigRomTreeEntry] {
        guard case .directory(let entries) = value else { return [] }
        return entries.map(ASFWMCPConfigRomTreeEntry.init(entry:))
    }

    private static func valueDescription(_ value: RomValue) -> String? {
        switch value {
        case .immediate(let word):
            String(format: "0x%06X", word)
        case .csrOffset(let address):
            String(format: "0x%012llX", address)
        case .leafPlaceholder(let byteOffset):
            "not fetched from partial cache (target byte \(byteOffset))"
        case .leafDescriptorText(let text, _):
            "text: \(text)"
        case .leafEUI64(let value):
            String(format: "0x%016llX", value)
        case .leafData(let data):
            "\(data.count) raw leaf bytes omitted"
        case .directory:
            nil
        }
    }
}

// The dispatch result builders for these tools live in ASFWMCPToolDispatch.swift
// alongside `ASFWMCPToolArgumentDecoder`, which is file-private there. This file
// owns the models and their MCP value projections.
