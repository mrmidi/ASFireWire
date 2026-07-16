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
