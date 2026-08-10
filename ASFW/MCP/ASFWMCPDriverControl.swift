import Foundation

protocol ASFWDriverControlling {
    func fetchTelemetrySnapshot(configuration: ASFWMCPRuntimeConfiguration) async -> ASFWMCPTelemetrySnapshot
    func fetchTopology() async -> ASFWMCPTopologySnapshot?
    func fetchConfigROM(nodeId: UInt32, generation: UInt32) async -> ASFWMCPConfigRomSummary?
    func fetchOhciSnapshot() async -> ASFWMCPOhciSnapshot?
    func fetchIrmAllocations() async -> ASFWMCPIrmAllocationReport?
    func recentFcpRecords(limit: Int) async -> [ASFWMCPFcpRecord]
    func listNodes() async -> [ASFWMCPNodeSummary]
    func listAVCUnits() async -> [ASFWMCPAVCUnitSummary]
    func avcSubunitCapabilities(guid: UInt64, type: UInt8, id: UInt8) async -> ASFWMCPAVCSubunitCapabilities?
    func listRecentTransactions(limit: Int) async -> [ASFWMCPTransactionEvent]
    func executeReadQuadlet(_ request: ASFWMCPReadQuadletRequest) async -> ASFWMCPTransactionResult
    func executeReadBlock(_ request: ASFWMCPReadBlockRequest) async -> ASFWMCPTransactionResult
    func executeWriteQuadlet(_ request: ASFWMCPWriteQuadletRequest) async -> ASFWMCPTransactionResult
    func executeWriteBlock(_ request: ASFWMCPWriteBlockRequest) async -> ASFWMCPTransactionResult
    func executeCompareSwap(_ request: ASFWMCPCompareSwapRequest) async -> ASFWMCPTransactionResult
    func executeFCPCommand(_ request: ASFWMCPFcpCommandRequest) async -> ASFWMCPFcpCommandReceipt
    func executePhase88Streaming(targetGuid: UInt64, start: Bool) async -> ASFWMCPPhase88StreamingReceipt
    func executeBusReset(_ request: ASFWMCPBusResetRequest) async -> ASFWMCPBusResetReceipt
    func executeIRMSnapshot(_ request: ASFWMCPIrmSnapshotRequest) async -> ASFWMCPIrmResourceSnapshot
    func queryLogRecords(_ query: ASFWLogRingQuery) async -> ASFWLogRingQueryResponse?
    func logRingStats() async -> ASFWLogRingStats?
    func fetchAudioStreamHealth() async -> [ASFWMCPAudioStreamHealth]
    func fetchAudioCursors() async -> [ASFWMCPAudioCursorSnapshot]
}

actor MockASFWDriverControl: ASFWDriverControlling {
    private let nodes: [ASFWMCPNodeSummary]
    private let transactions: [ASFWMCPTransactionEvent]
    private var generation: UInt32
    private let driverConnected: Bool
    private let controllerState: String
    private let linkActive: Bool
    private let topologyValid: Bool
    private let droppedEventCount: UInt32
    private let timeoutCount: UInt32
    private var attemptedWriteCount: Int = 0
    private var duetInputFdf: UInt8 = 0x02
    private var duetOutputFdf: UInt8 = 0x02
    private var phase88Streaming = false

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

    func fetchTopology() async -> ASFWMCPTopologySnapshot? {
        guard topologyValid else { return nil }
        // A deterministic two-port chain: node 0 (local) child-linked to node 2 (root),
        // matching the mock's controller telemetry so fixtures stay self-consistent.
        return ASFWMCPTopologySnapshot(
            generation: generation,
            nodeCount: nodes.count,
            rootNodeId: 2,
            irmNodeId: 2,
            localNodeId: 0,
            gapCount: 63,
            busBase16: 0xFFC0,
            nodes: nodes.enumerated().map { index, node in
                ASFWMCPTopologyNode(
                    nodeId: Int(node.nodeId),
                    portCount: 1,
                    gapCount: 63,
                    speedMbps: 400,
                    linkActive: true,
                    contender: node.nodeId == 2,
                    isRoot: node.nodeId == 2,
                    initiatedReset: false,
                    parentPort: node.nodeId == 2 ? nil : 0,
                    ports: [
                        ASFWMCPTopologyPort(
                            port: 0,
                            state: node.nodeId == 2 ? "child" : "parent",
                            remoteNodeId: node.nodeId == 2 ? 0 : 2,
                            remotePort: index == 0 ? 0 : 0
                        )
                    ]
                )
            },
            warnings: []
        )
    }

    func fetchConfigROM(nodeId: UInt32, generation requestedGeneration: UInt32) async -> ASFWMCPConfigRomSummary? {
        guard let node = nodes.first(where: { $0.nodeId == nodeId }), node.configRomCached else {
            return nil
        }
        return ASFWMCPConfigRomSummary(
            nodeId: nodeId,
            requestedGeneration: requestedGeneration,
            resolvedGeneration: generation,
            exactGenerationMatch: requestedGeneration == generation,
            byteCount: 256,
            quadletCount: 64,
            rootDirectoryStartQuadlet: 5,
            parsed: true,
            parseNote: nil,
            guid: node.guid,
            busName: "1394",
            irmc: true,
            cmc: true,
            isc: true,
            bmc: false,
            maxRec: 10,
            linkSpeed: 2,
            vendorName: node.vendorName,
            modelName: node.modelName,
            vendorId: nil,
            modelId: nil,
            modalias: nil,
            units: [],
            diagnostics: [],
            bibFields: [],
            rawQuadlets: [],
            tree: []
        )
    }

    func fetchOhciSnapshot() async -> ASFWMCPOhciSnapshot? {
        guard driverConnected else { return nil }
        // Deterministic values: the offset is echoed into the low half so fixtures can
        // assert that a named read resolved to the register it claimed.
        return ASFWMCPOhciSnapshot(
            generation: generation,
            registers: ASFWMCPOhciRegisterMap.covered.map {
                ASFWMCPOhciRegister(name: $0.name, offset: $0.offset, value: 0x0001_0000 | $0.offset)
            }
        )
    }

    func fetchIrmAllocations() async -> ASFWMCPIrmAllocationReport? {
        guard driverConnected else { return nil }
        // All channels free except 31 (BROADCAST_CHANNEL), so fixtures exercise the
        // cleared-bit decode at the boundary between the two words.
        return ASFWMCPIrmAllocationReport(
            generation: generation,
            irmNodeId: 2,
            localIsIRM: false,
            readbackValid: true,
            bandwidthAvailable: 4915,
            channelsAvailable31_0: 0xFFFF_FFFE,
            channelsAvailable63_32: 0xFFFF_FFFF
        )
    }

    func recentFcpRecords(limit: Int) async -> [ASFWMCPFcpRecord] {
        []
    }

    func listNodes() async -> [ASFWMCPNodeSummary] {
        nodes
    }

    func listAVCUnits() async -> [ASFWMCPAVCUnitSummary] {
        nodes.compactMap { node in
            guard node.protocolHints.contains("avc"),
                  let guidText = node.guid,
                  let guid = UInt64(guidText.dropFirst(2), radix: 16),
                  let vendorText = node.vendorId,
                  let vendorId = UInt32(vendorText.dropFirst(2), radix: 16),
                  let modelText = node.modelId,
                  let modelId = UInt32(modelText.dropFirst(2), radix: 16) else {
                return nil
            }
            return ASFWMCPAVCUnitSummary(
                guid: guid, nodeId: node.nodeId, vendorId: vendorId, modelId: modelId,
                isoInputPlugCount: 1, isoOutputPlugCount: 1,
                externalInputPlugCount: 1, externalOutputPlugCount: 1,
                subunits: [.init(type: 0x0C, id: 0, sourcePlugCount: 1, destinationPlugCount: 1)]
            )
        }
    }

    func avcSubunitCapabilities(guid: UInt64, type: UInt8, id: UInt8) async -> ASFWMCPAVCSubunitCapabilities? {
        guard (await listAVCUnits()).contains(where: { $0.guid == guid &&
            $0.subunits.contains(where: { $0.type == type && $0.id == id })
        }) else {
            return nil
        }
        return ASFWMCPAVCSubunitCapabilities(
            hasAudio: type == 0x0C || type == 0x01,
            hasMIDI: type == 0x0C,
            hasSMPTE: false,
            currentRateCode: 0x04,
            supportedRatesMask: (UInt32(1) << 3) | (UInt32(1) << 4),
            plugs: [.init(
                id: 0, isInput: true, type: 0x00, name: "Mock Audio",
                signalBlocks: [.init(formatCode: 0x06, channelCount: 2)],
                supportedFormats: [
                    .init(sampleRateCode: 0x03, formatCode: 0x06, channelCount: 2),
                    .init(sampleRateCode: 0x04, formatCode: 0x06, channelCount: 2),
                ]
            ), .init(
                id: 0, isInput: false, type: 0x00, name: "Mock Audio",
                signalBlocks: [.init(formatCode: 0x06, channelCount: 2)],
                supportedFormats: [
                    .init(sampleRateCode: 0x03, formatCode: 0x06, channelCount: 2),
                    .init(sampleRateCode: 0x04, formatCode: 0x06, channelCount: 2),
                ]
            )]
        )
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
        let payload = request.payload
        if request.intent == .status,
           payload == [0x01, 0xFF, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00] {
            return ASFWMCPFcpCommandReceipt(
                targetGUID: request.targetGUID,
                expectedNodeId: request.address.nodeId,
                expectedGeneration: request.address.generation,
                observedNodeId: request.address.nodeId,
                observedGeneration: generation,
                response: [0x0C, 0xFF, 0x02, 0x00, 0x01, 0x01, 0x00, 0x00],
                status: .ok,
                correlationId: "mock-fcp-bebob-unit-plug-info",
                durationUsec: 200,
                policy: nil
            )
        }
        if request.intent == .status,
           payload == [0x01, 0x60, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00] {
            return ASFWMCPFcpCommandReceipt(
                targetGUID: request.targetGUID,
                expectedNodeId: request.address.nodeId,
                expectedGeneration: request.address.generation,
                observedNodeId: request.address.nodeId,
                observedGeneration: generation,
                response: [0x0C, 0x60, 0x02, 0x00, 0x01, 0x00, 0x00, 0x00],
                status: .ok,
                correlationId: "mock-fcp-bebob-msu-plug-info",
                durationUsec: 200,
                policy: nil
            )
        }
        if request.intent == .status,
           payload == [0x01, 0x60, 0x02, 0xC0, 0x00, 0x01, 0x00, 0xFF, 0xFF, 0x00, 0x00, 0x00] {
            return ASFWMCPFcpCommandReceipt(
                targetGUID: request.targetGUID,
                expectedNodeId: request.address.nodeId,
                expectedGeneration: request.address.generation,
                observedNodeId: request.address.nodeId,
                observedGeneration: generation,
                response: [0x0C, 0x60, 0x02, 0xC0, 0x00, 0x01, 0x00, 0xFF, 0xFF, 0x00, 0x03],
                status: .ok,
                correlationId: "mock-fcp-bebob-msu-sync-type",
                durationUsec: 200,
                policy: nil
            )
        }
        if request.intent == .status,
           payload == [0x01, 0x60, 0x02, 0xC0, 0x00, 0x01, 0x00, 0xFF,
                       0xFF, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00] {
            return ASFWMCPFcpCommandReceipt(
                targetGUID: request.targetGUID,
                expectedNodeId: request.address.nodeId,
                expectedGeneration: request.address.generation,
                observedNodeId: request.address.nodeId,
                observedGeneration: generation,
                response: [0x0C, 0x60, 0x02, 0xC0, 0x00, 0x01, 0x00, 0xFF, 0xFF, 0x05,
                           0xFF, 0xFF, 0xFF, 0xFF, 0xFF],
                status: .ok,
                correlationId: "mock-fcp-bebob-msu-sync-source",
                durationUsec: 200,
                policy: nil
            )
        }
        if payload.count >= 6, payload[1] == 0xFF, payload[2] == 0x18 || payload[2] == 0x19 {
            let isInput = payload[2] == 0x19
            if request.intent == .status {
                return ASFWMCPFcpCommandReceipt(
                    targetGUID: request.targetGUID,
                    expectedNodeId: request.address.nodeId,
                    expectedGeneration: request.address.generation,
                    observedNodeId: request.address.nodeId,
                    observedGeneration: generation,
                    response: [0x0C, 0xFF, payload[2], 0x00, 0x90, isInput ? duetInputFdf : duetOutputFdf, 0xFF, 0xFF],
                    status: .ok,
                    correlationId: "mock-fcp-unit-plug-status",
                    durationUsec: 200,
                    policy: nil
                )
            }
            if request.intent == .control {
                if isInput { duetInputFdf = payload[5] } else { duetOutputFdf = payload[5] }
            }
        }
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

    func executePhase88Streaming(targetGuid: UInt64, start: Bool) async -> ASFWMCPPhase88StreamingReceipt {
        attemptedWriteCount += 1
        let matched = nodes.contains { $0.guid == String(format: "0x%016llX", targetGuid) &&
            $0.protocolHints.contains("bebob") && $0.protocolHints.contains("cmp") }
        guard matched else {
            return ASFWMCPPhase88StreamingReceipt(targetGuid: targetGuid, started: start, status: -536_870_201)
        }
        phase88Streaming = start
        return ASFWMCPPhase88StreamingReceipt(targetGuid: targetGuid, started: start, status: 0)
    }

    func executeBusReset(_ request: ASFWMCPBusResetRequest) async -> ASFWMCPBusResetReceipt {
        let correlationId = "mock-bus-reset"
        guard request.generation == generation else {
            return ASFWMCPBusResetReceipt(
                requestedGeneration: request.generation,
                acceptedGeneration: nil,
                observedGeneration: generation,
                shortReset: request.shortReset,
                status: .staleGeneration,
                correlationId: correlationId,
                durationUsec: nil,
                policy: nil
            )
        }

        attemptedWriteCount += 1
        let acceptedGeneration = generation
        generation &+= 1
        return ASFWMCPBusResetReceipt(
            requestedGeneration: request.generation,
            acceptedGeneration: acceptedGeneration,
            observedGeneration: generation,
            shortReset: request.shortReset,
            status: .ok,
            correlationId: correlationId,
            durationUsec: 500,
            policy: nil
        )
    }

    func executeIRMSnapshot(_ request: ASFWMCPIrmSnapshotRequest) async -> ASFWMCPIrmResourceSnapshot {
        let correlationId = "mock-irm-snapshot"
        guard request.generation == generation else {
            return ASFWMCPIrmResourceSnapshot(
                requestedGeneration: request.generation,
                observedGeneration: generation,
                irmNodeId: 2,
                bandwidthAvailable: nil,
                channelsAvailable31_0: nil,
                channelsAvailable63_32: nil,
                status: .staleGeneration,
                correlationId: correlationId,
                durationUsec: nil
            )
        }
        return ASFWMCPIrmResourceSnapshot(
            requestedGeneration: request.generation,
            observedGeneration: generation,
            irmNodeId: 2,
            bandwidthAvailable: 0x0000_1333,
            channelsAvailable31_0: 0xFFFF_FFFE,
            channelsAvailable63_32: 0xFFFF_FFFF,
            status: .ok,
            correlationId: correlationId,
            durationUsec: 300
        )
    }

    func queryLogRecords(_ query: ASFWLogRingQuery) async -> ASFWLogRingQueryResponse? {
        let allRecords = [
            ASFWLogRingRecord(
                sequence: 40, timestampNs: 1_000, category: 13, level: 2,
                message: "[CMP] iPCR connected channel=5"
            ),
            ASFWLogRingRecord(
                sequence: 41, timestampNs: 1_100, category: 5, level: 0,
                message: "[Async] AT Request context stopped"
            ),
            ASFWLogRingRecord(
                sequence: 42, timestampNs: 1_200, category: 16, level: 3,
                message: "[Audio] stream start complete"
            ),
        ]
        let filtered = allRecords.filter { record in
            record.sequence > query.afterSequence &&
            (query.categoryMask & (UInt32(1) << UInt32(record.category))) != 0 &&
            record.level <= query.maxLevel &&
            (query.contains.isEmpty || record.message.contains(query.contains))
        }
        let records = Array(filtered.prefix(query.maxRecords))
        return ASFWLogRingQueryResponse(
            records: records,
            nextSequence: records.last?.sequence ?? min(query.afterSequence + 8, 42),
            latestSequence: 42,
            oldestSequence: 1,
            scannedCount: UInt32(min(8, allRecords.count))
        )
    }

    func logRingStats() async -> ASFWLogRingStats? {
        ASFWLogRingStats(
            totalEmitted: 42,
            droppedRecords: 0,
            latestSequence: 42,
            oldestSequence: 1,
            capacityRecords: 40_000,
            perCategory: ["CMP": 18, "Async": 9, "Audio": 15]
        )
    }

    func fetchAudioStreamHealth() async -> [ASFWMCPAudioStreamHealth] {
        // A device that is streaming cleanly: every packet carries data and the
        // replay ring is being fed.
        [ASFWMCPAudioStreamHealth(
            guid: 0x0011_2233_4455_6677,
            bindingReady: true,
            streaming: true,
            sampleRateHz: 48_000,
            inputChannels: 14,
            outputChannels: 2,
            packetsSeen: 8_000,
            dataPackets: 7_936,
            noDataPackets: 64,
            emptyCompletions: 0,
            shortPackets: 0,
            invalidCipHeaders: 0,
            zeroDataBlockSize: 0,
            geometryMismatch: 0,
            replayEntries: 7_936,
            replayEpochResets: 1
        )]
    }

    func fetchAudioCursors() async -> [ASFWMCPAudioCursorSnapshot] {
        [ASFWMCPAudioCursorSnapshot(
            guid: 0x0011_2233_4455_6677,
            bindingReady: true,
            streaming: true,
            sampleRateHz: 48_000,
            outputChannels: 2,
            stagedOldestFrame: 10_000,
            stagedWrittenEndFrame: 14_096,
            finalizedFrameEnd: 13_984,
            completionPacket: 8_000,
            committedPacketEnd: 8_678,
            transportStatus: 1,
            stagingWrites: 11,
            stagingFrames: 5_632,
            stagingDiscontinuities: 0,
            stagingOverwrittenFrames: 0,
            readsReady: 1_748,
            readsNotYetWritten: 3,
            readsStaleOverwritten: 0,
            readsSnapshotBusy: 0,
            readsInvalid: 0,
            deferrals: 3,
            deadlineNoData: 0,
            staleXruns: 0,
            rebases: 0,
            faultEvents: 0,
            firstFaultReason: 0,
            firstFaultPacket: 0,
            firstFaultAudioFrame: 0,
            firstFaultOldestFrame: 0,
            firstFaultWrittenEndFrame: 0,
            firstFaultCompletionPacket: 0,
            firstFaultCommittedPacketEnd: 0
        )]
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
        switch address.addressLow {
        case 0xf000_0900, 0xf000_0980:
            // S400 MPR with two available PCRs.
            return 0x8000_0002
        case 0xf000_0904:
            // Online oPCR[0], one P2P consumer, channel 5, S400, overhead 3.
            return 0x8105_8c00
        case 0xf000_0984:
            // Online iPCR[0], one P2P consumer, channel 6.
            return 0x8106_0000
        default:
            break
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

    static let bebobNode = ASFWMCPNodeSummary(
        nodeId: 3,
        address16: "0xFFC3",
        guid: "0x000AAC0300B1D1F7",
        vendorId: "0x000AAC",
        modelId: "0x000003",
        vendorName: "TerraTec Electronic GmbH",
        modelName: "PHASE 88 Rack FW",
        configRomCached: true,
        protocolHints: ["avc", "bebob", "cmp"]
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
