import Foundation

enum ASFWMCPRuntimeMode: String, Equatable {
    case disabled
    case mock
    case readOnlyDeveloper
    case developerWriteEnabled
}

enum ASFWMCPVisibility: String, Equatable {
    case always
    case readOnly
    case developerWrite
    case rawDeveloper
}

enum ASFWMCPErrorCode: String, Equatable {
    case driverNotConnected
    case mcpDisabled
    case unsupportedRuntimeMode
    case capabilityUnavailable
    case capabilityChanged
    case policyDenied
    case dryRunOnly
    case requiresDeveloperMode
    case testGateMissing
    case unsupportedAddressSpace
    case staleGeneration
    case busResetDuringOperation
    case transactionTimeout
    case rcodeError
    case compareFailed
    case unsupportedProtocol
    case malformedRequest
    case payloadTooLarge
    case rawDataOmitted
}

enum ASFWMCPValue: Equatable {
    case null
    case bool(Bool)
    case int(Int)
    case uint64(UInt64)
    case string(String)
    case array([ASFWMCPValue])
    case object([String: ASFWMCPValue])
}

struct ASFWMCPResourceError: Equatable {
    let code: ASFWMCPErrorCode
    let reason: String
}

struct ASFWMCPResourceEnvelope: Equatable {
    let schema: String
    let uri: String
    let snapshotId: String
    let capturedAt: Date?
    let monotonicNs: UInt64?
    let generation: UInt32?
    let driverConnected: Bool
    let stale: Bool
    let truncated: Bool
    let data: ASFWMCPValue
    let links: [String]
    let errors: [ASFWMCPResourceError]
}

struct ASFWMCPToolDefinition: Equatable {
    let name: String
    let group: String
    let visibility: ASFWMCPVisibility
    let readOnly: Bool
    let idempotent: Bool
    let summary: String
}

struct ASFWMCPResourceDefinition: Equatable {
    let uri: String
    let schema: String
    let summary: String
}

struct ASFWMCPRuntimeConfiguration: Equatable {
    var mode: ASFWMCPRuntimeMode
    var writePolicyAvailable: Bool
    var swiftTestGatePassed: Bool
    var rawDeveloperTierEnabled: Bool

    static let disabled = ASFWMCPRuntimeConfiguration(
        mode: .disabled,
        writePolicyAvailable: false,
        swiftTestGatePassed: false,
        rawDeveloperTierEnabled: false
    )

    static let mock = ASFWMCPRuntimeConfiguration(
        mode: .mock,
        writePolicyAvailable: false,
        swiftTestGatePassed: false,
        rawDeveloperTierEnabled: false
    )

    static let readOnlyDeveloper = ASFWMCPRuntimeConfiguration(
        mode: .readOnlyDeveloper,
        writePolicyAvailable: false,
        swiftTestGatePassed: false,
        rawDeveloperTierEnabled: false
    )

    var canListDeveloperWriteTools: Bool {
        mode == .developerWriteEnabled && writePolicyAvailable && swiftTestGatePassed
    }

    var canListRawDeveloperTools: Bool {
        canListDeveloperWriteTools && rawDeveloperTierEnabled
    }
}

struct ASFWMCPPolicyTelemetry: Equatable {
    let runtimeMode: ASFWMCPRuntimeMode
    let writesListed: Bool
    let writeGate: String
}

struct ASFWMCPControllerTelemetry: Equatable {
    let state: String
    let linkActive: Bool
    let localNodeId: UInt32?
    let rootNodeId: UInt32?
    let irmNodeId: UInt32?
    let isIRM: Bool
    let isCycleMaster: Bool
}

struct ASFWMCPBusTelemetry: Equatable {
    let generation: UInt32
    let nodeCount: UInt32
    let busResetCount: UInt64
    let gapCount: UInt32
    let topologyValid: Bool
}

struct ASFWMCPAsyncTelemetry: Equatable {
    let recentEventCount: UInt32
    let droppedEventCount: UInt32
    let timeouts: UInt32
    let lastCompletionNs: UInt64?
}

struct ASFWMCPProtocolTelemetry: Equatable {
    let avcUnits: UInt32
    let sbp2Units: UInt32
    let diceTcatNodes: UInt32
    let cmpCapableNodes: UInt32
}

struct ASFWMCPNodeSummary: Equatable {
    let nodeId: UInt32
    let address16: String
    let guid: String?
    let vendorId: String?
    let modelId: String?
    let vendorName: String?
    let modelName: String?
    let configRomCached: Bool
    let protocolHints: [String]
}

struct ASFWMCPTransactionEvent: Equatable {
    let timestampNs: UInt64
    let generation: UInt32
    let direction: String
    let context: String
    let tLabel: UInt32
    let tCode: String
    let sourceId: String
    let destinationId: String
    let address: String
    let payloadBytes: UInt32
    let ackCode: String
    let rCode: String
    let speed: String
    let matchedTransaction: Bool
    let dropReason: String?
}

struct ASFWMCPTelemetrySnapshot: Equatable {
    let snapshotId: String
    let capturedAt: Date?
    let monotonicNs: UInt64?
    let generation: UInt32
    let driverConnected: Bool
    let controller: ASFWMCPControllerTelemetry
    let bus: ASFWMCPBusTelemetry
    let async: ASFWMCPAsyncTelemetry
    let protocols: ASFWMCPProtocolTelemetry
    let policy: ASFWMCPPolicyTelemetry
}

struct ASFWMCPHardwareSmokeStep: Equatable {
    let name: String
    let resourceURI: String?
    let toolName: String?
    let mutatesHardware: Bool
    let requiresExplicitEnablement: Bool
}

struct ASFWMCPHardwareSmokePlan: Equatable {
    let steps: [ASFWMCPHardwareSmokeStep]

    var containsMutatingOperations: Bool {
        steps.contains { $0.mutatesHardware }
    }
}
