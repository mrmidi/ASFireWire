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

struct ASFWMCPConfigRomSummary: Equatable {
    let nodeId: UInt32
    let requestedGeneration: UInt32
    let resolvedGeneration: UInt32
    /// False means the driver served a cache entry from a different generation.
    /// Node ids from another generation are not interchangeable with this one.
    let exactGenerationMatch: Bool
    let byteCount: Int
    let quadletCount: Int
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
    let units: [ASFWMCPConfigRomUnit]
    let diagnostics: [String]
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
            parsed: false,
            parseNote: note,
            guid: guid,
            busName: busName,
            irmc: irmc, cmc: cmc, isc: isc, bmc: bmc,
            maxRec: maxRec, linkSpeed: linkSpeed,
            vendorName: vendorName, modelName: modelName,
            units: units,
            diagnostics: diagnostics
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

extension ASFWMCPConfigRomSummary {
    var mcpValue: ASFWMCPValue {
        .object([
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
        ])
    }
}

// The dispatch result builders for these tools live in ASFWMCPToolDispatch.swift
// alongside `ASFWMCPToolArgumentDecoder`, which is file-private there. This file
// owns the models and their MCP value projections.
