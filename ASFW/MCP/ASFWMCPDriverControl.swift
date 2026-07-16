import Foundation

protocol ASFWDriverControlling {
    func fetchTelemetrySnapshot(configuration: ASFWMCPRuntimeConfiguration) async -> ASFWMCPTelemetrySnapshot
    func listNodes() async -> [ASFWMCPNodeSummary]
    func listRecentTransactions(limit: Int) async -> [ASFWMCPTransactionEvent]
    func executeReadQuadlet(_ request: ASFWMCPReadQuadletRequest) async -> ASFWMCPTransactionResult
    func executeReadBlock(_ request: ASFWMCPReadBlockRequest) async -> ASFWMCPTransactionResult
    func executeWriteQuadlet(_ request: ASFWMCPWriteQuadletRequest) async -> ASFWMCPTransactionResult
    func executeWriteBlock(_ request: ASFWMCPWriteBlockRequest) async -> ASFWMCPTransactionResult
    func executeCompareSwap(_ request: ASFWMCPCompareSwapRequest) async -> ASFWMCPTransactionResult
    func executeFCPCommand(_ request: ASFWMCPFcpCommandRequest) async -> ASFWMCPFcpCommandReceipt
}

actor MockASFWDriverControl: ASFWDriverControlling {
    private let nodes: [ASFWMCPNodeSummary]
    private let transactions: [ASFWMCPTransactionEvent]
    private let generation: UInt32
    private let driverConnected: Bool
    private let controllerState: String
    private let linkActive: Bool
    private let topologyValid: Bool
    private let droppedEventCount: UInt32
    private let timeoutCount: UInt32
    private var attemptedWriteCount: Int = 0

    init(
        generation: UInt32 = 17,
        nodes: [ASFWMCPNodeSummary] = MockASFWDriverControl.defaultNodes,
        transactions: [ASFWMCPTransactionEvent] = MockASFWDriverControl.defaultTransactions,
        driverConnected: Bool = true,
        controllerState: String = "Running",
        linkActive: Bool = true,
        topologyValid: Bool = true,
        droppedEventCount: UInt32 = 0,
        timeoutCount: UInt32 = 0
    ) {
        self.generation = generation
        self.nodes = nodes
        self.transactions = transactions
        self.driverConnected = driverConnected
        self.controllerState = controllerState
        self.linkActive = linkActive
        self.topologyValid = topologyValid
        self.droppedEventCount = droppedEventCount
        self.timeoutCount = timeoutCount
    }

    func fetchTelemetrySnapshot(configuration: ASFWMCPRuntimeConfiguration) async -> ASFWMCPTelemetrySnapshot {
        ASFWMCPTelemetrySnapshot(
            snapshotId: "mock-\(generation)",
            capturedAt: nil,
            monotonicNs: 123_456_789_000,
            generation: generation,
            driverConnected: driverConnected,
            controller: ASFWMCPControllerTelemetry(
                state: controllerState,
                linkActive: linkActive,
                localNodeId: 0,
                rootNodeId: 2,
                irmNodeId: 2,
                isIRM: false,
                isCycleMaster: false
            ),
            bus: ASFWMCPBusTelemetry(
                generation: generation,
                nodeCount: UInt32(nodes.count),
                busResetCount: 12,
                gapCount: 63,
                topologyValid: topologyValid
            ),
            async: ASFWMCPAsyncTelemetry(
                recentEventCount: UInt32(transactions.count),
                droppedEventCount: droppedEventCount,
                timeouts: timeoutCount,
                lastCompletionNs: transactions.last?.timestampNs
            ),
            protocols: ASFWMCPProtocolTelemetry(
                avcUnits: UInt32(nodes.filter { $0.protocolHints.contains("avc") }.count),
                sbp2Units: UInt32(nodes.filter { $0.protocolHints.contains("sbp2") }.count),
                diceTcatNodes: UInt32(nodes.filter { $0.protocolHints.contains("dice_tcat") }.count),
                cmpCapableNodes: UInt32(nodes.filter { $0.protocolHints.contains("cmp") }.count)
            ),
            policy: ASFWMCPPolicyTelemetry(
                runtimeMode: configuration.mode,
                writesListed: configuration.canListDeveloperWriteTools,
                writeGate: configuration.canListDeveloperWriteTools ? "open" : "testGateMissing"
            )
        )
    }

    func listNodes() async -> [ASFWMCPNodeSummary] {
        nodes
    }

    func listRecentTransactions(limit: Int) async -> [ASFWMCPTransactionEvent] {
        Array(transactions.prefix(max(0, limit)))
    }

    func executeReadQuadlet(_ request: ASFWMCPReadQuadletRequest) async -> ASFWMCPTransactionResult {
        ASFWMCPTransactionResult(
            kind: request.kind,
            ok: true,
            status: .ok,
            generation: generation,
            correlationId: "mock-read-quadlet",
            rCode: "complete",
            durationUsec: 100,
            payload: quadletBytes(mockQuadletValue(for: request.address))
        )
    }

    func executeReadBlock(_ request: ASFWMCPReadBlockRequest) async -> ASFWMCPTransactionResult {
        if request.validationError != nil {
            return .malformed(kind: request.kind, correlationId: "mock-read-block-malformed", generation: generation)
        }
        let pattern = quadletBytes(mockQuadletValue(for: request.address))
        let payload = (0..<Int(request.length)).map { pattern[$0 % pattern.count] }
        return ASFWMCPTransactionResult(
            kind: request.kind,
            ok: true,
            status: .ok,
            generation: generation,
            correlationId: "mock-read-block",
            rCode: "complete",
            durationUsec: 120,
            payload: payload
        )
    }

    func executeWriteQuadlet(_ request: ASFWMCPWriteQuadletRequest) async -> ASFWMCPTransactionResult {
        attemptedWriteCount += 1
        return ASFWMCPTransactionResult(
            kind: request.kind,
            ok: true,
            status: .ok,
            generation: generation,
            correlationId: "mock-write-quadlet",
            rCode: "complete",
            durationUsec: 140,
            payload: request.verifyReadback ? quadletBytes(request.value) : nil
        )
    }

    func executeWriteBlock(_ request: ASFWMCPWriteBlockRequest) async -> ASFWMCPTransactionResult {
        attemptedWriteCount += 1
        if request.validationError != nil {
            return .malformed(kind: request.kind, correlationId: "mock-write-block-malformed", generation: generation)
        }
        return ASFWMCPTransactionResult(
            kind: request.kind,
            ok: true,
            status: .ok,
            generation: generation,
            correlationId: "mock-write-block",
            rCode: "complete",
            durationUsec: 160,
            payload: request.verifyReadback ? request.payload : nil
        )
    }

    func executeCompareSwap(_ request: ASFWMCPCompareSwapRequest) async -> ASFWMCPTransactionResult {
        attemptedWriteCount += 1
        let comparePassed = request.expected == mockQuadletValue(for: request.address)
        return ASFWMCPTransactionResult(
            kind: request.kind,
            ok: comparePassed,
            status: comparePassed ? .ok : .compareFailed,
            generation: generation,
            correlationId: "mock-compare-swap",
            rCode: comparePassed ? "complete" : "conflictError",
            durationUsec: 180,
            payload: quadletBytes(comparePassed ? request.swap : mockQuadletValue(for: request.address))
        )
    }

    func executeFCPCommand(_ request: ASFWMCPFcpCommandRequest) async -> ASFWMCPFcpCommandReceipt {
        let matchingNode = nodes.first {
            $0.nodeId == request.address.nodeId &&
            $0.guid == String(format: "0x%016llX", request.targetGUID) &&
            $0.protocolHints.contains("avc")
        }
        guard matchingNode != nil else {
            return ASFWMCPFcpCommandReceipt(
                targetGUID: request.targetGUID,
                expectedNodeId: request.address.nodeId,
                expectedGeneration: request.address.generation,
                observedNodeId: nil,
                observedGeneration: generation,
                response: nil,
                status: .unavailable,
                correlationId: "mock-fcp-route-missing",
                durationUsec: nil,
                policy: nil
            )
        }
        guard request.address.generation == generation else {
            return ASFWMCPFcpCommandReceipt(
                targetGUID: request.targetGUID,
                expectedNodeId: request.address.nodeId,
                expectedGeneration: request.address.generation,
                observedNodeId: request.address.nodeId,
                observedGeneration: generation,
                response: nil,
                status: .staleGeneration,
                correlationId: "mock-fcp-stale-generation",
                durationUsec: nil,
                policy: nil
            )
        }
        attemptedWriteCount += 1
        return ASFWMCPFcpCommandReceipt(
            targetGUID: request.targetGUID,
            expectedNodeId: request.address.nodeId,
            expectedGeneration: request.address.generation,
            observedNodeId: request.address.nodeId,
            observedGeneration: generation,
            response: [0x0C, request.payload.dropFirst().first ?? 0xFF, request.payload.dropFirst(2).first ?? 0xFF],
            status: .ok,
            correlationId: "mock-fcp-command",
            durationUsec: 200,
            policy: nil
        )
    }

    func recordUnexpectedWriteAttempt() {
        attemptedWriteCount += 1
    }

    func unexpectedWriteAttemptCount() -> Int {
        attemptedWriteCount
    }

    private func mockQuadletValue(for address: ASFWMCPAddress) -> UInt32 {
        if address.offset48 == 0xFFFF_F000_0400 {
            return 0x3133_3934
        }
        return address.addressLow
    }

    private func quadletBytes(_ value: UInt32) -> [UInt8] {
        [
            UInt8((value >> 24) & 0xFF),
            UInt8((value >> 16) & 0xFF),
            UInt8((value >> 8) & 0xFF),
            UInt8(value & 0xFF)
        ]
    }

    static let defaultNodes: [ASFWMCPNodeSummary] = [
        ASFWMCPNodeSummary(
            nodeId: 0,
            address16: "0xFFC0",
            guid: "0x0011223344556677",
            vendorId: "0x0003DB",
            modelId: "0x01DDDD",
            vendorName: "Apogee",
            modelName: "Duet",
            configRomCached: true,
            protocolHints: ["avc", "cmp"]
        ),
        ASFWMCPNodeSummary(
            nodeId: 1,
            address16: "0xFFC1",
            guid: "0x00AABBCCDDEEFF00",
            vendorId: "0x00130E",
            modelId: "0x00000001",
            vendorName: "TCAT",
            modelName: "DICE",
            configRomCached: true,
            protocolHints: ["dice_tcat"]
        )
    ]

    static let sbp2Node = ASFWMCPNodeSummary(
        nodeId: 2,
        address16: "0xFFC2",
        guid: "0x0022334455667788",
        vendorId: "0x00609E",
        modelId: "0x00001000",
        vendorName: "Mock SBP-2",
        modelName: "Storage",
        configRomCached: true,
        protocolHints: ["sbp2"]
    )

    static let defaultTransactions: [ASFWMCPTransactionEvent] = [
        ASFWMCPTransactionEvent(
            timestampNs: 123_456_780_000,
            generation: 17,
            direction: "tx",
            context: "ATRequest",
            tLabel: 42,
            tCode: "readQuadlet",
            sourceId: "0xFFC0",
            destinationId: "0xFFC1",
            address: "0xFFFFF0000400",
            payloadBytes: 4,
            ackCode: "complete",
            rCode: "complete",
            speed: "S400",
            matchedTransaction: true,
            dropReason: nil
        )
    ]
}
