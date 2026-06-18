import Foundation

protocol ASFWDriverControlling {
    func fetchTelemetrySnapshot(configuration: ASFWMCPRuntimeConfiguration) async -> ASFWMCPTelemetrySnapshot
    func listNodes() async -> [ASFWMCPNodeSummary]
    func listRecentTransactions(limit: Int) async -> [ASFWMCPTransactionEvent]
}

actor MockASFWDriverControl: ASFWDriverControlling {
    private let nodes: [ASFWMCPNodeSummary]
    private let transactions: [ASFWMCPTransactionEvent]
    private let generation: UInt32
    private var attemptedWriteCount: Int = 0

    init(
        generation: UInt32 = 17,
        nodes: [ASFWMCPNodeSummary] = MockASFWDriverControl.defaultNodes,
        transactions: [ASFWMCPTransactionEvent] = MockASFWDriverControl.defaultTransactions
    ) {
        self.generation = generation
        self.nodes = nodes
        self.transactions = transactions
    }

    func fetchTelemetrySnapshot(configuration: ASFWMCPRuntimeConfiguration) async -> ASFWMCPTelemetrySnapshot {
        ASFWMCPTelemetrySnapshot(
            snapshotId: "mock-\(generation)",
            capturedAt: nil,
            monotonicNs: 123_456_789_000,
            generation: generation,
            driverConnected: true,
            controller: ASFWMCPControllerTelemetry(
                state: "Running",
                linkActive: true,
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
                topologyValid: true
            ),
            async: ASFWMCPAsyncTelemetry(
                recentEventCount: UInt32(transactions.count),
                droppedEventCount: 0,
                timeouts: 0,
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

    func recordUnexpectedWriteAttempt() {
        attemptedWriteCount += 1
    }

    func unexpectedWriteAttemptCount() -> Int {
        attemptedWriteCount
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
