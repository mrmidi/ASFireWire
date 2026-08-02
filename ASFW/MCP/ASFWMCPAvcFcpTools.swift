import Foundation

// FW-82: AV/C and FCP tools (MCP_TOOL_TAXONOMY.md §5.6).
//
// AV/C unit/subunit inspection and inquiry/status FCP commands are read-only.
// Control/notify/vendor-dependent FCP commands mutate device state and are
// policy-gated developer-write. All AV/C tools require the "avc" protocol hint,
// so they only appear for AV/C-capable nodes. AV/C frames are big-endian byte
// payloads written to the target's FCP command register (units space).

extension ASFWMCPToolCatalog {
    static let avcFcpTools: [ASFWMCPToolDefinition] = [
        ASFWMCPToolDefinition(name: "asfw_avc_list_units", group: "avc_fcp", visibility: .readOnly, readOnly: true, idempotent: true, summary: "List AV/C units, subunits, and plugs.", requiredProtocolHints: ["avc"]),
        ASFWMCPToolDefinition(name: "asfw_avc_get_subunit_capabilities", group: "avc_fcp", visibility: .readOnly, readOnly: true, idempotent: true, summary: "Return decoded AV/C subunit capabilities.", requiredProtocolHints: ["avc"]),
        ASFWMCPToolDefinition(name: "asfw_avc_get_subunit_descriptor", group: "avc_fcp", visibility: .readOnly, readOnly: true, idempotent: true, summary: "Return bounded AV/C subunit descriptor bytes and parsed summary.", requiredProtocolHints: ["avc"]),
        ASFWMCPToolDefinition(name: "asfw_fcp_send_command", group: "avc_fcp", visibility: .readOnly, readOnly: true, idempotent: false, summary: "Send an inquiry/status-only FCP/AV/C command.", requiredProtocolHints: ["avc"]),
        ASFWMCPToolDefinition(name: "asfw_fcp_get_recent_responses", group: "avc_fcp", visibility: .readOnly, readOnly: true, idempotent: true, summary: "Inspect recent FCP command/response records.", requiredProtocolHints: ["avc"]),
        ASFWMCPToolDefinition(name: "asfw_apogee_duet_apply_format_dev", group: "avc_fcp", visibility: .developerWrite, readOnly: false, idempotent: false, summary: "Guardedly apply and verify an Apogee Duet OXFW AM824 format transition.", requiredProtocolHints: ["avc"]),
        ASFWMCPToolDefinition(name: "asfw_fcp_send_command_dev", group: "avc_fcp", visibility: .developerWrite, readOnly: false, idempotent: false, summary: "Developer-tier raw FCP command that may mutate device state.", requiredProtocolHints: ["avc"])
    ]
}

/// AV/C command intent (`ctype`). Only inquiry/status are non-mutating.
enum ASFWMCPAvcCommandIntent: String, Equatable, CaseIterable {
    case inquiry
    case status
    case control
    case notify
    case vendorDependent

    var isMutating: Bool {
        switch self {
        case .inquiry, .status:
            return false
        case .control, .notify, .vendorDependent:
            return true
        }
    }
}

/// Generation-bound AV/C discovery evidence exposed to MCP. This deliberately
/// mirrors only decoded driver state; it is not a request to probe hardware.
struct ASFWMCPAVCUnitSummary: Equatable {
    struct Subunit: Equatable {
        let type: UInt8
        let id: UInt8
        let sourcePlugCount: UInt8
        let destinationPlugCount: UInt8
    }

    let guid: UInt64
    let nodeId: UInt32
    let vendorId: UInt32
    let modelId: UInt32
    let isoInputPlugCount: UInt8
    let isoOutputPlugCount: UInt8
    let externalInputPlugCount: UInt8
    let externalOutputPlugCount: UInt8
    let subunits: [Subunit]
}

struct ASFWMCPAVCSubunitCapabilities: Equatable {
    struct Plug: Equatable {
        struct SignalBlock: Equatable {
            let formatCode: UInt8
            let channelCount: UInt8
        }

        struct SupportedFormat: Equatable {
            let sampleRateCode: UInt8
            let formatCode: UInt8
            let channelCount: UInt8
        }

        let id: UInt8
        let isInput: Bool
        let type: UInt8
        let name: String
        let signalBlocks: [SignalBlock]
        let supportedFormats: [SupportedFormat]
    }

    let hasAudio: Bool
    let hasMIDI: Bool
    let hasSMPTE: Bool
    let currentRateCode: UInt8
    let supportedRatesMask: UInt32
    let plugs: [Plug]
}

extension ASFWMCPAVCUnitSummary {
    var mcpValue: ASFWMCPValue {
        .object([
            "guid": .string(String(format: "0x%016llX", guid)),
            "nodeId": .int(Int(nodeId)),
            "vendorId": .string(String(format: "0x%06X", vendorId)),
            "modelId": .string(String(format: "0x%06X", modelId)),
            "plugs": .object([
                "isoInput": .int(Int(isoInputPlugCount)),
                "isoOutput": .int(Int(isoOutputPlugCount)),
                "externalInput": .int(Int(externalInputPlugCount)),
                "externalOutput": .int(Int(externalOutputPlugCount)),
            ]),
            "subunits": .array(subunits.map {
                .object([
                    "type": .int(Int($0.type)),
                    "id": .int(Int($0.id)),
                    "sourcePlugCount": .int(Int($0.sourcePlugCount)),
                    "destinationPlugCount": .int(Int($0.destinationPlugCount)),
                ])
            }),
        ])
    }
}

extension ASFWMCPAVCSubunitCapabilities {
    var mcpValue: ASFWMCPValue {
        .object([
            "hasAudio": .bool(hasAudio),
            "hasMIDI": .bool(hasMIDI),
            "hasSMPTE": .bool(hasSMPTE),
            "currentRateCode": .int(Int(currentRateCode)),
            "supportedRatesMask": .uint64(UInt64(supportedRatesMask)),
            "plugs": .array(plugs.map { plug in
                .object([
                    "id": .int(Int(plug.id)),
                    "isInput": .bool(plug.isInput),
                    "type": .int(Int(plug.type)),
                    "name": .string(plug.name),
                    "signalBlocks": .array(plug.signalBlocks.map {
                        .object([
                            "formatCode": .int(Int($0.formatCode)),
                            "channelCount": .int(Int($0.channelCount)),
                        ])
                    }),
                    "supportedFormats": .array(plug.supportedFormats.map {
                        .object([
                            "sampleRateCode": .int(Int($0.sampleRateCode)),
                            "formatCode": .int(Int($0.formatCode)),
                            "channelCount": .int(Int($0.channelCount)),
                        ])
                    }),
                ])
            }),
        ])
    }
}

/// A raw FCP/AV/C command directed at a node's FCP command register.
struct ASFWMCPFcpCommandRequest: Equatable {
    /// Stable device identity selected by the caller. The live adapter resolves
    /// it to `address.nodeId` immediately before issuing the command.
    let targetGUID: UInt64
    /// Target node's FCP command register address.
    let address: ASFWMCPAddress
    let intent: ASFWMCPAvcCommandIntent
    /// AV/C frame bytes in bus (big-endian) order.
    let payload: [UInt8]

    /// FCP frames are bounded to 512 bytes.
    static let maxPayload = 512

    var validationError: ASFWMCPErrorCode? {
        if payload.count > Self.maxPayload { return .payloadTooLarge }
        if payload.isEmpty { return .malformedRequest }
        return nil
    }

    /// Policy request for mutating intents, or nil for inquiry/status reads,
    /// which are not gated by write policy.
    func policyRequest(
        currentGeneration: UInt32,
        protocolSupported: Bool = true,
        dryRun: Bool = false
    ) -> ASFWMCPPolicyRequest? {
        guard intent.isMutating else { return nil }
        return .forTransaction(
            kind: .writeBlock,
            address: address,
            currentGeneration: currentGeneration,
            protocolHint: "avc",
            protocolSupported: protocolSupported,
            dryRun: dryRun
        )
    }

    /// Read-only MCP calls must prove that the AV/C ctype byte agrees with
    /// their declared intent. This prevents a caller from labelling a CONTROL
    /// frame as STATUS to bypass the developer-write gate.
    var hasMatchingReadOnlyCType: Bool {
        guard let ctype = payload.first else { return false }
        switch intent {
        case .status:
            return ctype == 0x01
        case .inquiry:
            return ctype == 0x02
        case .control, .notify, .vendorDependent:
            return false
        }
    }
}

/// Receipt for a developer FCP command. `observedNodeId` and
/// `observedGeneration` describe the preflight route validated by this MCP
/// slice. FW-100 will replace those observations with the transport's exact
/// write-completion attempt context before this is used as a response matcher
/// diagnostic.
struct ASFWMCPFcpCommandReceipt: Equatable {
    let targetGUID: UInt64
    let expectedNodeId: UInt32
    let expectedGeneration: UInt32
    let observedNodeId: UInt32?
    let observedGeneration: UInt32
    let response: [UInt8]?
    let status: ASFWMCPTransactionStatus
    let correlationId: String
    let durationUsec: UInt64?
    let policy: ASFWMCPPolicyDecision?

    var ok: Bool { status == .ok }
}

extension ASFWMCPFcpCommandReceipt {
    var mcpValue: ASFWMCPValue {
        .object([
            "kind": .string("fcpCommand"),
            "ok": .bool(ok),
            "status": .string(status.rawValue),
            "targetGuid": .string(String(format: "0x%016llX", targetGUID)),
            "expectedNodeId": .int(Int(expectedNodeId)),
            "expectedGeneration": .int(Int(expectedGeneration)),
            "observedNodeId": observedNodeId.map { .int(Int($0)) } ?? .null,
            "observedGeneration": .int(Int(observedGeneration)),
            "correlationId": .string(correlationId),
            "durationUsec": durationUsec.map { .uint64($0) } ?? .null,
            "response": response.map { .array($0.map { .int(Int($0)) }) } ?? .null,
            "policy": policy.map(\.mcpValue) ?? .null
        ])
    }
}
