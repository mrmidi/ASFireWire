import Foundation

// FW-79: MCP write policy engine and refusal reasons.
//
// Classifies write-capable MCP requests by operation type, address space,
// protocol surface, runtime/developer mode, and node generation, returning a
// structured decision. The single safety invariant is
// `ASFWMCPPolicyDecision.reachesDriverWritePath`: only an `allowed` decision may
// reach the driver/user-client write path. Every refusal and every dry run is
// false, so denied and dry-run writes cannot touch hardware.
//
// Decision and error vocabulary mirror documentation/MCP_TOOL_TAXONOMY.md §11.
// The gate extension at the bottom of this file is the FW-89 test-gate tightening
// called for in documentation/MCP_TEST_GATE.md §6.

/// Coarse policy classification of a FireWire address.
enum ASFWMCPAddressSpace: String, Equatable {
    /// IEEE 1212 CSR-architecture register block (0xFFFF_F000_0000 ..< 0x0400).
    case csrCore
    /// Config ROM space (0xFFFF_F000_0400 ..< 0x0800) — architecturally read-only.
    case configRom
    /// Initial units / unit-dependent register space (>= 0xFFFF_F000_0800).
    case unitsSpace
    /// Below the CSR block: posted/physical memory on the target.
    case physicalMemory
    /// Local OHCI controller registers (host-side, not a node address).
    case ohciController
    /// Address could not be classified.
    case unknown
}

/// Mutation class of a request for policy purposes.
enum ASFWMCPOperationType: String, Equatable {
    case read
    case write
    case compareSwap

    var isMutating: Bool { self != .read }
}

/// Structured policy decision categories (taxonomy §11).
enum ASFWMCPWriteDecision: String, Equatable, CaseIterable {
    case allowed
    case denied
    case dryRunOnly
    case requiresDeveloperMode
    case unsupportedAddressSpace
    case staleGeneration
    case unsupportedProtocol
}

/// A policy decision plus its human- and machine-readable justification.
struct ASFWMCPPolicyDecision: Equatable {
    let decision: ASFWMCPWriteDecision
    /// Human-readable explanation, including how to make the operation valid when
    /// that is possible.
    let reason: String
    /// Machine-readable error code aligned with the MCP error vocabulary.
    let errorCode: ASFWMCPErrorCode?
    /// Runtime mode the caller must reach for this operation to be allowed.
    let requiredMode: ASFWMCPRuntimeMode?
    /// Capability/tier the caller must enable (e.g. "rawDeveloperTier").
    let requiredCapability: String?

    init(
        decision: ASFWMCPWriteDecision,
        reason: String,
        errorCode: ASFWMCPErrorCode? = nil,
        requiredMode: ASFWMCPRuntimeMode? = nil,
        requiredCapability: String? = nil
    ) {
        self.decision = decision
        self.reason = reason
        self.errorCode = errorCode
        self.requiredMode = requiredMode
        self.requiredCapability = requiredCapability
    }

    /// The load-bearing safety invariant: only `allowed` may reach the driver
    /// write path. Every refusal and every dry run is false.
    var reachesDriverWritePath: Bool {
        decision == .allowed
    }

    var isDryRun: Bool {
        decision == .dryRunOnly
    }
}

/// The inputs the policy engine classifies.
struct ASFWMCPPolicyRequest: Equatable {
    let operationType: ASFWMCPOperationType
    let addressSpace: ASFWMCPAddressSpace
    /// Protocol surface the request rides (e.g. "cmp", "dice_tcat"), or nil for
    /// raw async address-space access.
    let protocolHint: String?
    /// Whether a node in the current generation advertises that protocol. Ignored
    /// when `protocolHint` is nil.
    let protocolSupported: Bool
    /// Generation the request is pinned to.
    let requestedGeneration: UInt32
    /// Current live bus generation.
    let currentGeneration: UInt32
    /// Caller explicitly asked for a dry run (classify + validate, never execute).
    let dryRun: Bool
    /// The request targets a raw developer-tier escape hatch (e.g. OHCI write).
    let requiresRawDeveloperTier: Bool

    init(
        operationType: ASFWMCPOperationType,
        addressSpace: ASFWMCPAddressSpace,
        requestedGeneration: UInt32,
        currentGeneration: UInt32,
        protocolHint: String? = nil,
        protocolSupported: Bool = true,
        dryRun: Bool = false,
        requiresRawDeveloperTier: Bool = false
    ) {
        self.operationType = operationType
        self.addressSpace = addressSpace
        self.requestedGeneration = requestedGeneration
        self.currentGeneration = currentGeneration
        self.protocolHint = protocolHint
        self.protocolSupported = protocolSupported
        self.dryRun = dryRun
        self.requiresRawDeveloperTier = requiresRawDeveloperTier
    }
}

/// Stateless engine that maps a policy request to a structured decision.
struct ASFWMCPWritePolicyEngine {
    let configuration: ASFWMCPRuntimeConfiguration

    func evaluate(_ request: ASFWMCPPolicyRequest) -> ASFWMCPPolicyDecision {
        // Reads are never gated by write policy.
        if request.operationType == .read {
            return ASFWMCPPolicyDecision(
                decision: .allowed,
                reason: "Read operations are not gated by write policy."
            )
        }

        // Mock mode never reaches a live driver: it validates write shape and
        // classifies, but resolves any authorized write to a dry run. Live modes
        // must clear the developer-write gate before classification proceeds.
        if configuration.mode != .mock {
            guard configuration.mode == .developerWriteEnabled else {
                return ASFWMCPPolicyDecision(
                    decision: .requiresDeveloperMode,
                    reason: "Writes require developerWriteEnabled mode; current mode is \(configuration.mode.rawValue).",
                    errorCode: .requiresDeveloperMode,
                    requiredMode: .developerWriteEnabled
                )
            }
            guard configuration.canListDeveloperWriteTools else {
                return ASFWMCPPolicyDecision(
                    decision: .requiresDeveloperMode,
                    reason: "developerWriteEnabled requires an available write policy and a passing Swift test gate before writes are permitted.",
                    errorCode: .testGateMissing,
                    requiredMode: .developerWriteEnabled
                )
            }
            if request.requiresRawDeveloperTier && configuration.canListRawDeveloperTools == false {
                return ASFWMCPPolicyDecision(
                    decision: .denied,
                    reason: "This is a raw developer-tier escape hatch; enable the raw developer tier to proceed.",
                    errorCode: .policyDenied,
                    requiredMode: .developerWriteEnabled,
                    requiredCapability: "rawDeveloperTier"
                )
            }
        }

        // Classification applies in both mock and live-gated contexts.
        guard request.requestedGeneration == request.currentGeneration else {
            return ASFWMCPPolicyDecision(
                decision: .staleGeneration,
                reason: "Request generation \(request.requestedGeneration) does not match current bus generation \(request.currentGeneration); re-read topology and retry.",
                errorCode: .staleGeneration
            )
        }

        switch request.addressSpace {
        case .configRom:
            return ASFWMCPPolicyDecision(
                decision: .unsupportedAddressSpace,
                reason: "Config ROM space is architecturally read-only and cannot be written.",
                errorCode: .unsupportedAddressSpace
            )
        case .unknown:
            return ASFWMCPPolicyDecision(
                decision: .unsupportedAddressSpace,
                reason: "Target address could not be classified into a writable space.",
                errorCode: .unsupportedAddressSpace
            )
        case .csrCore, .unitsSpace, .physicalMemory, .ohciController:
            break
        }

        if let hint = request.protocolHint, request.protocolSupported == false {
            return ASFWMCPPolicyDecision(
                decision: .unsupportedProtocol,
                reason: "No connected node in the current generation supports the \(hint) protocol surface.",
                errorCode: .unsupportedProtocol
            )
        }

        // Mock mode or an explicit dry-run request: authorized shape, not executed.
        if configuration.mode == .mock || request.dryRun {
            return ASFWMCPPolicyDecision(
                decision: .dryRunOnly,
                reason: configuration.mode == .mock
                    ? "Mock mode validates the write shape but never reaches a driver."
                    : "Caller requested a dry run; the write was authorized but not executed.",
                errorCode: .dryRunOnly
            )
        }

        return ASFWMCPPolicyDecision(
            decision: .allowed,
            reason: "Write authorized under developerWriteEnabled policy."
        )
    }
}

extension ASFWMCPCore {
    /// The write policy engine bound to this core's runtime configuration.
    var writePolicyEngine: ASFWMCPWritePolicyEngine {
        ASFWMCPWritePolicyEngine(configuration: configuration)
    }

    /// Evaluate a write/CAS request against the current policy.
    func evaluateWritePolicy(_ request: ASFWMCPPolicyRequest) -> ASFWMCPPolicyDecision {
        writePolicyEngine.evaluate(request)
    }
}
