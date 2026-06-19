import Foundation

// FW-84: SBP-2 inspection and developer control tools (MCP_TOOL_TAXONOMY.md §5.8).
//
// Inspection (units, unit directory, session status) is read-only. Login and ORB
// submission are intentionally lower priority and treated as raw developer-tier
// escape hatches (visibility matrix §7: "hidden unless raw dev tier enabled"),
// so they require both the write gate and the raw developer tier. All tools use
// the "sbp2" protocol hint.

extension ASFWMCPToolCatalog {
    static let sbp2Tools: [ASFWMCPToolDefinition] = [
        ASFWMCPToolDefinition(name: "asfw_sbp2_list_units", group: "sbp2", visibility: .readOnly, readOnly: true, idempotent: true, summary: "List SBP-2 units discovered from Config ROM.", requiredProtocolHints: ["sbp2"]),
        ASFWMCPToolDefinition(name: "asfw_sbp2_inspect_unit", group: "sbp2", visibility: .readOnly, readOnly: true, idempotent: true, summary: "Decode an SBP-2 unit directory and command-set hints.", requiredProtocolHints: ["sbp2"]),
        ASFWMCPToolDefinition(name: "asfw_sbp2_get_session_status", group: "sbp2", visibility: .readOnly, readOnly: true, idempotent: true, summary: "Report login/session/fetch-agent state.", requiredProtocolHints: ["sbp2"]),
        ASFWMCPToolDefinition(name: "asfw_sbp2_login_dev", group: "sbp2", visibility: .rawDeveloper, readOnly: false, idempotent: false, summary: "Raw developer-tier SBP-2 login.", requiredProtocolHints: ["sbp2"]),
        ASFWMCPToolDefinition(name: "asfw_sbp2_submit_orb_dev", group: "sbp2", visibility: .rawDeveloper, readOnly: false, idempotent: false, summary: "Raw developer-tier SBP-2 ORB submission.", requiredProtocolHints: ["sbp2"])
    ]
}

/// Distinguishes the inspection states an SBP-2 query can report.
enum ASFWMCPSbp2SessionState: String, Equatable, CaseIterable {
    /// No SBP-2 device present at the queried node.
    case absent
    /// Node present but the unit is not a supported SBP-2 target.
    case unsupported
    /// Supported unit with no active login/session.
    case inactive
    /// Logged in with an active fetch agent.
    case active
    /// Unit directory or session decode failed.
    case protocolError
}

struct ASFWMCPSbp2SessionStatus: Equatable {
    let nodeId: UInt32
    let state: ASFWMCPSbp2SessionState
    /// Set only when state == .active.
    let loginId: UInt32?

    init(nodeId: UInt32, state: ASFWMCPSbp2SessionState, loginId: UInt32? = nil) {
        self.nodeId = nodeId
        self.state = state
        self.loginId = loginId
    }
}

struct ASFWMCPSbp2LoginRequest: Equatable {
    /// Address of the target unit's management agent.
    let address: ASFWMCPAddress

    func policyRequest(currentGeneration: UInt32, protocolSupported: Bool = true, dryRun: Bool = false) -> ASFWMCPPolicyRequest {
        .forTransaction(
            kind: .writeBlock,
            address: address,
            currentGeneration: currentGeneration,
            protocolHint: "sbp2",
            protocolSupported: protocolSupported,
            dryRun: dryRun,
            requiresRawDeveloperTier: true
        )
    }
}

struct ASFWMCPSbp2OrbRequest: Equatable {
    let address: ASFWMCPAddress
    /// ORB bytes in bus (big-endian) order.
    let orb: [UInt8]

    var validationError: ASFWMCPErrorCode? {
        if orb.isEmpty { return .malformedRequest }
        if orb.count % 4 != 0 { return .malformedRequest }
        if orb.count > Int(ASFWMCPTransactionLimits.maxBlockBytes) { return .payloadTooLarge }
        return nil
    }

    func policyRequest(currentGeneration: UInt32, protocolSupported: Bool = true, dryRun: Bool = false) -> ASFWMCPPolicyRequest {
        .forTransaction(
            kind: .writeBlock,
            address: address,
            currentGeneration: currentGeneration,
            protocolHint: "sbp2",
            protocolSupported: protocolSupported,
            dryRun: dryRun,
            requiresRawDeveloperTier: true
        )
    }
}
