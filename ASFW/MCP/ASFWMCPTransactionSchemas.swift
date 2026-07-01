import Foundation

// FW-78: foundational MCP schemas for FireWire async transactions.
//
// These are pure value types modelling the request/result contract documented in
// documentation/MCP_TOOL_TAXONOMY.md §5.3 (async transactions) and §5.5 (CAS).
// They carry no live driver access. Mutating requests (write/CAS) must be cleared
// by the FW-79 write policy engine before any of them may reach the
// driver/user-client write path — see ASFWMCPWritePolicy.swift.
//
// Endianness: IEEE 1394 wire payloads are big-endian. Quadlet `value`/`expected`/
// `swap` fields are expressed in host byte order and are serialized big-endian
// onto the bus by the live driver layer; block `payload`/`payload` bytes are
// already in bus (big-endian) order.

/// A typed 48-bit FireWire target address plus the addressing context required to
/// issue an async transaction against it.
struct ASFWMCPAddress: Equatable, Sendable {
    /// 16-bit bus/node identifier (`bus_id` << 6 | `phy_id`) of the target node.
    let nodeId: UInt32
    /// Bus generation the request is pinned to. A mismatch against the live
    /// generation is a `staleGeneration` refusal (see FW-79).
    let generation: UInt32
    /// High 16 bits of the 48-bit address.
    let addressHigh: UInt16
    /// Low 32 bits of the 48-bit address.
    let addressLow: UInt32

    /// Full 48-bit address offset.
    var offset48: UInt64 {
        (UInt64(addressHigh) << 32) | UInt64(addressLow)
    }
}

/// The async transaction primitives ASFW models for MCP.
enum ASFWMCPTransactionKind: String, Equatable, Sendable {
    case readQuadlet
    case readBlock
    case writeQuadlet
    case writeBlock
    case compareSwap

    /// Whether the primitive mutates target state and therefore requires a write
    /// policy decision before execution.
    var isMutating: Bool {
        switch self {
        case .readQuadlet, .readBlock:
            return false
        case .writeQuadlet, .writeBlock, .compareSwap:
            return true
        }
    }
}

/// Conservative schema bounds for async transactions.
enum ASFWMCPTransactionLimits {
    /// Async block payload ceiling used by schema validation. The IEEE 1394
    /// per-packet maximum scales with speed (2048 B @ S400, 4096 B @ S800); the
    /// schema validates against the S400 ceiling unless a caller raises it.
    static let maxBlockBytes: UInt32 = 2048
}

struct ASFWMCPReadQuadletRequest: Equatable, Sendable {
    let address: ASFWMCPAddress

    var kind: ASFWMCPTransactionKind { .readQuadlet }
}

struct ASFWMCPReadBlockRequest: Equatable, Sendable {
    let address: ASFWMCPAddress
    /// Requested length in bytes; must be a non-zero multiple of 4 within bounds.
    let length: UInt32

    var kind: ASFWMCPTransactionKind { .readBlock }

    /// Returns the schema violation for this request, or nil when well-formed.
    var validationError: ASFWMCPErrorCode? {
        if length > ASFWMCPTransactionLimits.maxBlockBytes { return .payloadTooLarge }
        if length == 0 || length % 4 != 0 { return .malformedRequest }
        return nil
    }
}

struct ASFWMCPWriteQuadletRequest: Equatable, Sendable {
    let address: ASFWMCPAddress
    /// Quadlet value in host byte order.
    let value: UInt32
    /// Issue a verifying read-back after the write when true.
    let verifyReadback: Bool

    init(address: ASFWMCPAddress, value: UInt32, verifyReadback: Bool = false) {
        self.address = address
        self.value = value
        self.verifyReadback = verifyReadback
    }

    var kind: ASFWMCPTransactionKind { .writeQuadlet }
}

struct ASFWMCPWriteBlockRequest: Equatable, Sendable {
    let address: ASFWMCPAddress
    /// Payload bytes in bus (big-endian) order.
    let payload: [UInt8]
    /// Issue a verifying read-back after the write when true.
    let verifyReadback: Bool

    init(address: ASFWMCPAddress, payload: [UInt8], verifyReadback: Bool = false) {
        self.address = address
        self.payload = payload
        self.verifyReadback = verifyReadback
    }

    var kind: ASFWMCPTransactionKind { .writeBlock }

    var validationError: ASFWMCPErrorCode? {
        if payload.count > Int(ASFWMCPTransactionLimits.maxBlockBytes) { return .payloadTooLarge }
        if payload.isEmpty || payload.count % 4 != 0 { return .malformedRequest }
        return nil
    }
}

/// Quadlet lock / compare-and-swap (taxonomy §5.5 `asfw_cas_quadlet`). Also the
/// primitive behind IRM and CMP mutations.
struct ASFWMCPCompareSwapRequest: Equatable, Sendable {
    let address: ASFWMCPAddress
    /// Expected current quadlet (host byte order).
    let expected: UInt32
    /// Value to store if the comparison matches (host byte order).
    let swap: UInt32

    var kind: ASFWMCPTransactionKind { .compareSwap }
}

/// Terminal status of a (possibly refused) async transaction.
enum ASFWMCPTransactionStatus: String, Equatable, Sendable {
    case ok
    case timeout
    case rcodeError
    case busReset
    case compareFailed
    case staleGeneration
    case unavailable
    /// Refused by the FW-79 write policy before reaching the driver.
    case denied
    /// Policy-cleared shape that was intentionally not executed.
    case dryRun
    /// Failed schema validation.
    case malformed
}

/// Stable async transaction result shape (taxonomy §5.3).
struct ASFWMCPTransactionResult: Equatable, Sendable {
    let kind: ASFWMCPTransactionKind
    let ok: Bool
    let status: ASFWMCPTransactionStatus
    /// FireWire response code mnemonic (e.g. "complete", "conflictError"), when a
    /// response was received.
    let rCode: String?
    /// Bus generation the transaction actually executed in.
    let generation: UInt32
    let durationUsec: UInt64?
    let correlationId: String
    /// Raw payload (bus order) for reads, or read-back bytes for verified writes.
    let payload: [UInt8]?
    /// Optional decoded view (e.g. parsed register fields) when ASFW can decode.
    let decoded: ASFWMCPValue?
    /// Policy decision attached to write-capable calls (FW-79). Nil for reads.
    let policy: ASFWMCPPolicyDecision?

    init(
        kind: ASFWMCPTransactionKind,
        ok: Bool,
        status: ASFWMCPTransactionStatus,
        generation: UInt32,
        correlationId: String,
        rCode: String? = nil,
        durationUsec: UInt64? = nil,
        payload: [UInt8]? = nil,
        decoded: ASFWMCPValue? = nil,
        policy: ASFWMCPPolicyDecision? = nil
    ) {
        self.kind = kind
        self.ok = ok
        self.status = status
        self.generation = generation
        self.correlationId = correlationId
        self.rCode = rCode
        self.durationUsec = durationUsec
        self.payload = payload
        self.decoded = decoded
        self.policy = policy
    }
}

extension ASFWMCPTransactionResult {
    /// Build the result for a write/CAS that the policy engine did not authorize
    /// for execution. Denials and dry runs share this shape; neither reaches the
    /// driver write path.
    static func policyRefusal(
        kind: ASFWMCPTransactionKind,
        correlationId: String,
        generation: UInt32,
        policy: ASFWMCPPolicyDecision
    ) -> ASFWMCPTransactionResult {
        ASFWMCPTransactionResult(
            kind: kind,
            ok: false,
            status: policy.isDryRun ? .dryRun : .denied,
            generation: generation,
            correlationId: correlationId,
            policy: policy
        )
    }

    /// Build the result for a request rejected by schema validation.
    static func malformed(
        kind: ASFWMCPTransactionKind,
        correlationId: String,
        generation: UInt32
    ) -> ASFWMCPTransactionResult {
        ASFWMCPTransactionResult(
            kind: kind,
            ok: false,
            status: .malformed,
            generation: generation,
            correlationId: correlationId
        )
    }
}

// MARK: - Bridge to the FW-79 write policy surface
//
// These map FW-78 transaction types onto the FW-79 policy inputs. They live with
// the schemas because they depend on the concrete transaction/address types; the
// policy engine itself classifies an already-resolved ASFWMCPPolicyRequest.

extension ASFWMCPAddressSpace {
    /// Classify a node-address into a coarse policy space. OHCI/controller
    /// registers are not node addresses, so `.ohciController` is supplied
    /// explicitly by the caller rather than derived here.
    static func classify(_ address: ASFWMCPAddress) -> ASFWMCPAddressSpace {
        let offset = address.offset48
        let csrBase: UInt64 = 0xFFFF_F000_0000
        let configRomBase: UInt64 = 0xFFFF_F000_0400
        let unitsBase: UInt64 = 0xFFFF_F000_0800
        if offset >= unitsBase { return .unitsSpace }
        if offset >= configRomBase { return .configRom }
        if offset >= csrBase { return .csrCore }
        return .physicalMemory
    }
}

extension ASFWMCPTransactionKind {
    var operationType: ASFWMCPOperationType {
        switch self {
        case .readQuadlet, .readBlock:
            return .read
        case .writeQuadlet, .writeBlock:
            return .write
        case .compareSwap:
            return .compareSwap
        }
    }
}

extension ASFWMCPPolicyRequest {
    /// Build a policy request for an async transaction against a node address,
    /// classifying its address space automatically.
    static func forTransaction(
        kind: ASFWMCPTransactionKind,
        address: ASFWMCPAddress,
        currentGeneration: UInt32,
        protocolHint: String? = nil,
        protocolSupported: Bool = true,
        dryRun: Bool = false,
        requiresRawDeveloperTier: Bool = false
    ) -> ASFWMCPPolicyRequest {
        ASFWMCPPolicyRequest(
            operationType: kind.operationType,
            addressSpace: ASFWMCPAddressSpace.classify(address),
            requestedGeneration: address.generation,
            currentGeneration: currentGeneration,
            protocolHint: protocolHint,
            protocolSupported: protocolSupported,
            dryRun: dryRun,
            requiresRawDeveloperTier: requiresRawDeveloperTier
        )
    }
}
