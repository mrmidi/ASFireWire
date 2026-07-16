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

/// Direction of a remote CMP plug control register.  The names are relative
/// to the remote device: oPCR is device-to-host, iPCR is host-to-device.
enum ASFWMCPCmpPcrDirection: String, Equatable {
    case output
    case input

    var registerBase: UInt32 {
        switch self {
        case .output: return 0xf000_0904
        case .input: return 0xf000_0984
        }
    }

    var streamDirection: String {
        switch self {
        case .output: return "deviceToHost"
        case .input: return "hostToDevice"
        }
    }
}

/// Decoded IEC 61883-1 PCR view.  It is intentionally a decoder only: CMP
/// connection ownership and compare-swap retries remain in the driver.
/// Field positions are cross-validated with Linux sound/firewire/cmp.c:22-42.
struct ASFWMCPCmpPcr: Equatable {
    let direction: ASFWMCPCmpPcrDirection
    let plug: UInt32
    let rawValue: UInt32

    var online: Bool { rawValue & 0x8000_0000 != 0 }
    var broadcastConnection: Bool { rawValue & 0x4000_0000 != 0 }
    var pointToPointConnections: UInt8 { UInt8((rawValue >> 24) & 0x3f) }
    var isInUse: Bool { broadcastConnection || pointToPointConnections != 0 }
    var channel: UInt8 { UInt8((rawValue >> 16) & 0x3f) }
    var dataRateCode: UInt8? {
        direction == .output ? UInt8((rawValue >> 14) & 0x03) : nil
    }
    var overheadId: UInt8? {
        direction == .output ? UInt8((rawValue >> 10) & 0x0f) : nil
    }

    static func address(for direction: ASFWMCPCmpPcrDirection, plug: UInt32) -> UInt32? {
        guard plug <= ASFWMCPCmpLimits.maxPlug else { return nil }
        return direction.registerBase + plug * 4
    }

    var mcpValue: ASFWMCPValue {
        var value: [String: ASFWMCPValue] = [
            "direction": .string(direction.rawValue),
            "streamDirection": .string(direction.streamDirection),
            "plug": .int(Int(plug)),
            "rawValue": .string(String(format: "0x%08X", rawValue)),
            "online": .bool(online),
            "inUse": .bool(isInUse),
            "broadcastConnection": .bool(broadcastConnection),
            "pointToPointConnections": .int(Int(pointToPointConnections)),
            "channel": .int(Int(channel)),
        ]
        if let dataRateCode {
            value["dataRateCode"] = .int(Int(dataRateCode))
            value["dataRate"] = .string(["S100", "S200", "S400", "S800"][Int(dataRateCode)])
        }
        if let overheadId {
            value["overheadId"] = .int(Int(overheadId))
        }
        return .object(value)
    }
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
