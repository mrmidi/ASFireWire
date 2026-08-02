import Foundation

// FW-118: explicitly gated local bus-reset control.
//
// This tool intentionally calls the driver's BusResetCoordinator rather than
// exposing PHY or OHCI reset registers.  The coordinator owns reset ordering,
// quiesce, and generation changes; MCP supplies only a generation-pinned,
// human-acknowledged developer trigger.

extension ASFWMCPToolCatalog {
    static let busResetDeveloperTools: [ASFWMCPToolDefinition] = [
        ASFWMCPToolDefinition(
            name: "asfw_bus_reset_dev",
            group: "bus_topology",
            visibility: .developerWrite,
            readOnly: false,
            idempotent: false,
            summary: "Request one generation-pinned local software bus reset; interrupts active streams."
        )
    ]
}

struct ASFWMCPBusResetRequest: Equatable {
    let generation: UInt32
    let shortReset: Bool

    func policyRequest(currentGeneration: UInt32, dryRun: Bool) -> ASFWMCPPolicyRequest {
        ASFWMCPPolicyRequest(
            operationType: .write,
            addressSpace: .ohciController,
            requestedGeneration: generation,
            currentGeneration: currentGeneration,
            dryRun: dryRun
        )
    }
}

struct ASFWMCPBusResetReceipt: Equatable {
    let requestedGeneration: UInt32
    let acceptedGeneration: UInt32?
    let observedGeneration: UInt32
    let shortReset: Bool
    let status: ASFWMCPTransactionStatus
    let correlationId: String
    let durationUsec: UInt64?
    let policy: ASFWMCPPolicyDecision?

    var ok: Bool { status == .ok }

    var mcpValue: ASFWMCPValue {
        .object([
            "kind": .string("busReset"),
            "ok": .bool(ok),
            "status": .string(status.rawValue),
            "requestedGeneration": .int(Int(requestedGeneration)),
            "acceptedGeneration": acceptedGeneration.map { .int(Int($0)) } ?? .null,
            "observedGeneration": .int(Int(observedGeneration)),
            "shortReset": .bool(shortReset),
            "correlationId": .string(correlationId),
            "durationUsec": durationUsec.map { .uint64($0) } ?? .null,
            "policy": policy.map(\.mcpValue) ?? .null
        ])
    }
}
