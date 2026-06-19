import Foundation

// FW-83: Connection Management Procedures (CMP) tools (MCP_TOOL_TAXONOMY.md §5.7).
//
// Plug listing and PCR reads are read-only inspection. PCR writes and
// connection establish/break are mutations: they are compare-swaps against the
// node's plug control registers (so they share the FW-78 CAS schema and FW-79
// policy), and use the "cmp" protocol hint for discovery. CMP writes prefer CAS
// so a stale plug value surfaces as compareFailed at execution.

extension ASFWMCPToolCatalog {
    static let cmpTools: [ASFWMCPToolDefinition] = [
        ASFWMCPToolDefinition(name: "asfw_cmp_list_plugs", group: "cmp", visibility: .readOnly, readOnly: true, idempotent: true, summary: "List known iPCR/oPCR plug state.", requiredProtocolHints: ["cmp"]),
        ASFWMCPToolDefinition(name: "asfw_cmp_read_pcr", group: "cmp", visibility: .readOnly, readOnly: true, idempotent: false, summary: "Read and decode a CMP plug control register.", requiredProtocolHints: ["cmp"]),
        ASFWMCPToolDefinition(name: "asfw_cmp_write_pcr", group: "cmp", visibility: .developerWrite, readOnly: false, idempotent: false, summary: "Policy-gated CMP PCR write (compare-swap).", requiredProtocolHints: ["cmp"]),
        ASFWMCPToolDefinition(name: "asfw_cmp_establish_connection", group: "cmp", visibility: .developerWrite, readOnly: false, idempotent: false, summary: "Policy-gated connection establishment.", requiredProtocolHints: ["cmp"]),
        ASFWMCPToolDefinition(name: "asfw_cmp_break_connection", group: "cmp", visibility: .developerWrite, readOnly: false, idempotent: false, summary: "Policy-gated connection break.", requiredProtocolHints: ["cmp"])
    ]
}

/// IEC 61883-1 plug control registers: PCR_0..PCR_30 per direction.
enum ASFWMCPCmpLimits {
    static let maxPlug: UInt32 = 30
}

struct ASFWMCPCmpPcrWriteRequest: Equatable {
    /// Address of the target plug control register.
    let address: ASFWMCPAddress
    /// Plug index (0...30).
    let plug: UInt32
    /// Expected current PCR value (host order).
    let expected: UInt32
    /// New PCR value (host order).
    let swap: UInt32

    var validationError: ASFWMCPErrorCode? {
        plug <= ASFWMCPCmpLimits.maxPlug ? nil : .malformedRequest
    }

    func policyRequest(currentGeneration: UInt32, protocolSupported: Bool = true, dryRun: Bool = false) -> ASFWMCPPolicyRequest {
        .forTransaction(
            kind: .compareSwap,
            address: address,
            currentGeneration: currentGeneration,
            protocolHint: "cmp",
            protocolSupported: protocolSupported,
            dryRun: dryRun
        )
    }
}

struct ASFWMCPCmpConnectionRequest: Equatable {
    /// Address of the plug control register backing the connection.
    let address: ASFWMCPAddress
    let plug: UInt32
    /// True to establish, false to break.
    let establish: Bool

    var validationError: ASFWMCPErrorCode? {
        plug <= ASFWMCPCmpLimits.maxPlug ? nil : .malformedRequest
    }

    func policyRequest(currentGeneration: UInt32, protocolSupported: Bool = true, dryRun: Bool = false) -> ASFWMCPPolicyRequest {
        .forTransaction(
            kind: .compareSwap,
            address: address,
            currentGeneration: currentGeneration,
            protocolHint: "cmp",
            protocolSupported: protocolSupported,
            dryRun: dryRun
        )
    }
}
