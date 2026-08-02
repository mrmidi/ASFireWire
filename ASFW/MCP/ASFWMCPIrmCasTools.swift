import Foundation

// FW-81: IRM and CAS tools (MCP_TOOL_TAXONOMY.md §5.5).
//
// IRM state is read-only inspection; channel/bandwidth allocation and the
// compare-swap primitive are mutations gated by the FW-79 write policy. CAS
// shares the FW-78 transaction request/result schema. IRM allocations are
// compare-swaps against the IRM node's CSR-core resource registers, so their
// policy requests classify as `.csrCore` / `.compareSwap`.

extension ASFWMCPToolCatalog {
    static let irmCasTools: [ASFWMCPToolDefinition] = [
        ASFWMCPToolDefinition(name: "asfw_irm_get_state", group: "irm_cas", visibility: .readOnly, readOnly: true, idempotent: true, summary: "Return IRM and bus manager state."),
        ASFWMCPToolDefinition(name: "asfw_irm_get_bandwidth", group: "irm_cas", visibility: .readOnly, readOnly: true, idempotent: true, summary: "Read isochronous bandwidth availability."),
        ASFWMCPToolDefinition(name: "asfw_irm_get_channels", group: "irm_cas", visibility: .readOnly, readOnly: true, idempotent: true, summary: "Read isochronous channel availability."),
        ASFWMCPToolDefinition(name: "asfw_irm_list_allocations", group: "irm_cas", visibility: .readOnly, readOnly: true, idempotent: true, summary: "List ASFW-known IRM allocations."),
        ASFWMCPToolDefinition(name: "asfw_cas_quadlet", group: "irm_cas", visibility: .developerWrite, readOnly: false, idempotent: false, summary: "Policy-gated compare-swap primitive."),
        ASFWMCPToolDefinition(name: "asfw_irm_allocate_channel", group: "irm_cas", visibility: .developerWrite, readOnly: false, idempotent: false, summary: "Policy-gated isochronous channel allocation."),
        ASFWMCPToolDefinition(name: "asfw_irm_free_channel", group: "irm_cas", visibility: .developerWrite, readOnly: false, idempotent: false, summary: "Policy-gated isochronous channel release."),
        ASFWMCPToolDefinition(name: "asfw_irm_allocate_bandwidth", group: "irm_cas", visibility: .developerWrite, readOnly: false, idempotent: false, summary: "Policy-gated isochronous bandwidth allocation."),
        ASFWMCPToolDefinition(name: "asfw_irm_free_bandwidth", group: "irm_cas", visibility: .developerWrite, readOnly: false, idempotent: false, summary: "Policy-gated isochronous bandwidth release.")
    ]
}

/// Read-only view of the three IRM resource CSRs. The values are collected as
/// individual quadlet transactions in the same order as IRMClient's existing
/// resource snapshot path, so this is observational rather than atomic.
struct ASFWMCPIrmSnapshotRequest: Equatable {
    let generation: UInt32
}

/// Read-only local IRM resource view supplied by the driver's diagnostics
/// snapshot. It is used only when ASFW itself owns the IRM role; issuing an
/// async transaction to the local node has no remote AR response to match.
struct ASFWMCPLocalIrmResourceSnapshot: Equatable {
    let generation: UInt32
    let localNodeId: UInt32
    let irmNodeId: UInt32
    let isLocalIRM: Bool
    let readbackValid: Bool
    let bandwidthAvailable: UInt32
    let channelsAvailable31_0: UInt32
    let channelsAvailable63_32: UInt32
}

struct ASFWMCPIrmResourceSnapshot: Equatable {
    let requestedGeneration: UInt32
    let observedGeneration: UInt32
    let irmNodeId: UInt32?
    let bandwidthAvailable: UInt32?
    let channelsAvailable31_0: UInt32?
    let channelsAvailable63_32: UInt32?
    let status: ASFWMCPTransactionStatus
    let correlationId: String
    let durationUsec: UInt64?

    var ok: Bool { status == .ok }

    var mcpValue: ASFWMCPValue {
        .object([
            "kind": .string("irmResourceSnapshot"),
            "ok": .bool(ok),
            "status": .string(status.rawValue),
            "requestedGeneration": .int(Int(requestedGeneration)),
            "observedGeneration": .int(Int(observedGeneration)),
            "irmNodeId": irmNodeId.map { .int(Int($0)) } ?? .null,
            "bandwidthAvailable": bandwidthAvailable.map { .uint64(UInt64($0)) } ?? .null,
            "channelsAvailable31_0": channelsAvailable31_0.map { .uint64(UInt64($0)) } ?? .null,
            "channelsAvailable63_32": channelsAvailable63_32.map { .uint64(UInt64($0)) } ?? .null,
            "atomic": .bool(false),
            "correlationId": .string(correlationId),
            "durationUsec": durationUsec.map { .uint64($0) } ?? .null
        ])
    }
}

// MARK: - CAS bridge to write policy

extension ASFWMCPCompareSwapRequest {
    func policyRequest(currentGeneration: UInt32, dryRun: Bool = false) -> ASFWMCPPolicyRequest {
        .forTransaction(kind: .compareSwap, address: address, currentGeneration: currentGeneration, dryRun: dryRun)
    }
}

// MARK: - IRM allocation requests

struct ASFWMCPIrmChannelRequest: Equatable {
    /// Isochronous channel number (0...63).
    let channel: UInt32
    /// Bus generation the allocation is pinned to.
    let generation: UInt32
    /// True to allocate, false to free.
    let allocate: Bool

    static let maxChannel: UInt32 = 63

    var validationError: ASFWMCPErrorCode? {
        channel <= Self.maxChannel ? nil : .malformedRequest
    }

    func policyRequest(currentGeneration: UInt32, dryRun: Bool = false) -> ASFWMCPPolicyRequest {
        ASFWMCPPolicyRequest(
            operationType: .compareSwap,
            addressSpace: .csrCore,
            requestedGeneration: generation,
            currentGeneration: currentGeneration,
            dryRun: dryRun
        )
    }
}

struct ASFWMCPIrmBandwidthRequest: Equatable {
    /// Isochronous bandwidth allocation units.
    let allocationUnits: UInt32
    let generation: UInt32
    let allocate: Bool

    /// IEEE 1394 BANDWIDTH_AVAILABLE caps total isochronous units.
    static let maxAllocationUnits: UInt32 = 0x1333

    var validationError: ASFWMCPErrorCode? {
        allocationUnits <= Self.maxAllocationUnits ? nil : .malformedRequest
    }

    func policyRequest(currentGeneration: UInt32, dryRun: Bool = false) -> ASFWMCPPolicyRequest {
        ASFWMCPPolicyRequest(
            operationType: .compareSwap,
            addressSpace: .csrCore,
            requestedGeneration: generation,
            currentGeneration: currentGeneration,
            dryRun: dryRun
        )
    }
}
