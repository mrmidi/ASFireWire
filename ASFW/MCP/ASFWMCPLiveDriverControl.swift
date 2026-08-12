import Foundation

@MainActor
protocol ASFWLiveDriverBackend: AnyObject {
    var mcpIsConnected: Bool { get }
    var mcpLastError: String? { get }

    func mcpCurrentGeneration() -> UInt32?
    func mcpControllerStatus() -> ControllerStatus?
    func mcpFetchDiagnostics() throws -> ASFWDiagnosticsSnapshot
    func mcpLocalIrmResourceSnapshot() -> ASFWMCPLocalIrmResourceSnapshot?
    func mcpDiscoveredDevices() -> [FWDeviceInfo]?
    func mcpTopologySnapshot() -> TopologySnapshot?
    func mcpConfigROM(deviceID: DeviceInstanceID,
                      expectedGeneration: UInt16) -> ASFWDriverConnector.ConfigROMFetchResult?
    func mcpAVCUnits() -> [AVCUnitInfo]?
    func mcpAVCSubunitCapabilities(unitID: UnitInstanceID, type: UInt8, id: UInt8) -> AVCMusicCapabilities?

    func mcpAsyncRead(deviceID: DeviceInstanceID, addressHigh: UInt16, addressLow: UInt32, length: UInt32) -> UInt16?
    func mcpAsyncWrite(deviceID: DeviceInstanceID, addressHigh: UInt16, addressLow: UInt32, payload: Data) -> UInt16?
    func mcpAsyncBlockRead(deviceID: DeviceInstanceID, addressHigh: UInt16, addressLow: UInt32, length: UInt32) -> UInt16?
    func mcpAsyncBlockWrite(deviceID: DeviceInstanceID, addressHigh: UInt16, addressLow: UInt32, payload: Data) -> UInt16?
    func mcpAsyncCompareSwap(deviceID: DeviceInstanceID, addressHigh: UInt16, addressLow: UInt32, compareValue: Data, newValue: Data) -> UInt16?
    func mcpTransactionResult(handle: UInt16, initialPayloadCapacity: Int) -> ASFWDriverConnector.AsyncTransactionResult?
    func mcpSendRawFCPCommand(unitID: UnitInstanceID, frame: Data, timeoutMs: UInt32) -> Data?
    func mcpSetAudioStreaming(endpointID: AudioEndpointID, enabled: Bool) -> Int32
    func mcpRequestUserBusReset(expectedGeneration: UInt32, shortReset: Bool) -> UInt32?
    func mcpQueryLogRecords(_ query: ASFWLogRingQuery) -> ASFWLogRingQueryResponse?
    func mcpLogRingStats() -> ASFWLogRingStats?
    func mcpAudioTelemetry() -> AudioTelemetrySnapshot?
}

extension ASFWDriverConnector: ASFWLiveDriverBackend {
    var mcpIsConnected: Bool { isConnected }
    var mcpLastError: String? { lastError }

    func mcpCurrentGeneration() -> UInt32? {
        getControllerStatus()?.generation
    }

    func mcpControllerStatus() -> ControllerStatus? {
        getControllerStatus()
    }

    func mcpFetchDiagnostics() throws -> ASFWDiagnosticsSnapshot {
        try ASFWDiagnosticsClient(connector: self).fetchSnapshot()
    }

    func mcpAudioTelemetry() -> AudioTelemetrySnapshot? {
        getAudioTelemetry()
    }

    func mcpLocalIrmResourceSnapshot() -> ASFWMCPLocalIrmResourceSnapshot? {
        guard let diagnostics = try? mcpFetchDiagnostics(),
              let localNodeId = diagnostics.busContract.localNode.nodeIdOrNil,
              let irmNodeId = diagnostics.busContract.irmNode.nodeIdOrNil else {
            return nil
        }

        let busManager = diagnostics.busManager
        return ASFWMCPLocalIrmResourceSnapshot(
            generation: diagnostics.busContract.header.generation,
            localNodeId: localNodeId,
            irmNodeId: irmNodeId,
            isLocalIRM: busManager.localIsIRM != 0,
            readbackValid: busManager.localIrmReadbackValid != 0,
            bandwidthAvailable: busManager.localIrmBandwidthAvailable,
            // CHANNELS_AVAILABLE_HI carries channels 0..31 and LO carries 32..63.
            // Proof in-tree: LocalIRMResourceController.cpp:81 tests
            // `channelsAvailableHi & 0x1` for the channel-31 broadcast reservation, so
            // channel N sits at bit (31 - N) of the HI word. These two were previously
            // swapped here, which inverted both halves of `asfw_irm_get_channels`.
            channelsAvailable31_0: busManager.localIrmChannelsAvailableHi,
            channelsAvailable63_32: busManager.localIrmChannelsAvailableLo
        )
    }

    func mcpDiscoveredDevices() -> [FWDeviceInfo]? {
        getDiscoveredDevices()
    }

    func mcpTopologySnapshot() -> TopologySnapshot? {
        getTopologySnapshot()
    }

    func mcpConfigROM(deviceID: DeviceInstanceID,
                      expectedGeneration: UInt16) -> ASFWDriverConnector.ConfigROMFetchResult? {
        getConfigROM(deviceID: deviceID, expectedGeneration: expectedGeneration)
    }

    func mcpAVCUnits() -> [AVCUnitInfo]? {
        getAVCUnits()
    }

    func mcpAVCSubunitCapabilities(unitID: UnitInstanceID, type: UInt8, id: UInt8) -> AVCMusicCapabilities? {
        getSubunitCapabilities(unitID: unitID, type: type, id: id)
    }

    func mcpAsyncRead(deviceID: DeviceInstanceID, addressHigh: UInt16, addressLow: UInt32, length: UInt32) -> UInt16? {
        asyncRead(deviceID: deviceID, addressHigh: addressHigh, addressLow: addressLow, length: length)
    }

    func mcpAsyncWrite(deviceID: DeviceInstanceID, addressHigh: UInt16, addressLow: UInt32, payload: Data) -> UInt16? {
        asyncWrite(deviceID: deviceID, addressHigh: addressHigh, addressLow: addressLow, payload: payload)
    }

    func mcpAsyncBlockRead(deviceID: DeviceInstanceID, addressHigh: UInt16, addressLow: UInt32, length: UInt32) -> UInt16? {
        asyncBlockRead(deviceID: deviceID, addressHigh: addressHigh, addressLow: addressLow, length: length)
    }

    func mcpAsyncBlockWrite(deviceID: DeviceInstanceID, addressHigh: UInt16, addressLow: UInt32, payload: Data) -> UInt16? {
        asyncBlockWrite(deviceID: deviceID, addressHigh: addressHigh, addressLow: addressLow, payload: payload)
    }

    func mcpAsyncCompareSwap(deviceID: DeviceInstanceID, addressHigh: UInt16, addressLow: UInt32, compareValue: Data, newValue: Data) -> UInt16? {
        asyncCompareSwap(
            deviceID: deviceID,
            addressHigh: addressHigh,
            addressLow: addressLow,
            compareValue: compareValue,
            newValue: newValue
        )?.handle
    }

    func mcpTransactionResult(handle: UInt16, initialPayloadCapacity: Int) -> ASFWDriverConnector.AsyncTransactionResult? {
        getTransactionResult(handle: handle, initialPayloadCapacity: initialPayloadCapacity)
    }

    func mcpSendRawFCPCommand(unitID: UnitInstanceID, frame: Data, timeoutMs: UInt32) -> Data? {
        sendRawFCPCommand(unitID: unitID, frame: frame, timeoutMs: timeoutMs)
    }

    func mcpSetAudioStreaming(endpointID: AudioEndpointID, enabled: Bool) -> Int32 {
        Int32(setAudioStreaming(endpointID: endpointID, enabled: enabled))
    }

    func mcpRequestUserBusReset(expectedGeneration: UInt32, shortReset: Bool) -> UInt32? {
        requestUserBusReset(expectedGeneration: expectedGeneration, shortReset: shortReset)
    }

    func mcpQueryLogRecords(_ query: ASFWLogRingQuery) -> ASFWLogRingQueryResponse? {
        queryLogRecords(query)
    }

    func mcpLogRingStats() -> ASFWLogRingStats? {
        logRingStats()
    }
}

@MainActor
final class LiveASFWDriverControl: ASFWDriverControlling {
    private let backend: any ASFWLiveDriverBackend
    private let transactionTimeout: TimeInterval
    private let pollIntervalNs: UInt64
    private let busResetTimeout: TimeInterval

    /// Bounded ring of MCP-issued FCP exchanges, newest last. Driver-originated FCP
    /// is not captured here — see `ASFWMCPFcpRecord`.
    private var fcpRecords: [ASFWMCPFcpRecord] = []
    private static let fcpRecordCapacity = 64

    private static func bigEndianQuadlets(from data: Data) -> [UInt32] {
        guard data.count >= 4 else { return [] }
        let end = data.count - (data.count % 4)
        var result: [UInt32] = []
        result.reserveCapacity(end / 4)
        for offset in stride(from: 0, to: end, by: 4) {
            let high = UInt32(data[offset]) << 24
            let upperMiddle = UInt32(data[offset + 1]) << 16
            let lowerMiddle = UInt32(data[offset + 2]) << 8
            let low = UInt32(data[offset + 3])
            result.append(high | upperMiddle | lowerMiddle | low)
        }
        return result
    }

    init(
        backend: any ASFWLiveDriverBackend,
        transactionTimeout: TimeInterval = 2.0,
        pollIntervalNs: UInt64 = 25_000_000,
        busResetTimeout: TimeInterval = 10.0
    ) {
        self.backend = backend
        self.transactionTimeout = transactionTimeout
        self.pollIntervalNs = pollIntervalNs
        self.busResetTimeout = busResetTimeout
    }

    func fetchTelemetrySnapshot(configuration: ASFWMCPRuntimeConfiguration) async -> ASFWMCPTelemetrySnapshot {
        let status = backend.mcpControllerStatus()
        let diagnostics = try? backend.mcpFetchDiagnostics()
        let nodes = listNodesFromBackend()
        let events = recentTransactions(from: diagnostics?.asyncTrace, limit: Int(ASFW_DIAG_MAX_ASYNC_EVENTS))
        let generation = diagnostics?.busContract.header.generation ?? status?.generation ?? backend.mcpCurrentGeneration() ?? 0
        let nodeCount = diagnostics?.busContract.nodeCount ?? status?.nodeCount ?? UInt32(nodes.count)
        let busResetCount = UInt64(diagnostics?.busContract.asfwInitiatedResetCount ?? 0)
        let topologyValid = diagnostics?.topology.valid != 0 || status != nil

        return ASFWMCPTelemetrySnapshot(
            snapshotId: backend.mcpIsConnected ? "live-\(generation)-\(DispatchTime.now().uptimeNanoseconds)" : "live-unavailable",
            capturedAt: Date(),
            monotonicNs: DispatchTime.now().uptimeNanoseconds,
            generation: generation,
            driverConnected: backend.mcpIsConnected,
            controller: ASFWMCPControllerTelemetry(
                state: status?.stateName ?? (backend.mcpIsConnected ? "Unknown" : "Disconnected"),
                linkActive: status?.nodeCount ?? 0 > 0,
                localNodeId: diagnostics?.busContract.localNode.nodeIdOrNil ?? status?.localNodeID.map(UInt32.init),
                rootNodeId: diagnostics?.busContract.rootNode.nodeIdOrNil ?? status?.rootNodeID.map(UInt32.init),
                irmNodeId: diagnostics?.busContract.irmNode.nodeIdOrNil ?? status?.irmNodeID.map(UInt32.init),
                isIRM: status?.isIRM ?? false,
                isCycleMaster: status?.isCycleMaster ?? false
            ),
            bus: ASFWMCPBusTelemetry(
                generation: generation,
                nodeCount: nodeCount,
                busResetCount: status?.busResetCount ?? busResetCount,
                gapCount: diagnostics?.busContract.gapCount ?? 0,
                topologyValid: topologyValid
            ),
            async: ASFWMCPAsyncTelemetry(
                recentEventCount: UInt32(events.count),
                droppedEventCount: diagnostics?.asyncTrace.droppedCount ?? 0,
                timeouts: UInt32(events.filter { $0.rCode == "timeout" || $0.dropReason == "timeout" }.count),
                lastCompletionNs: events.last?.timestampNs
            ),
            protocols: protocolTelemetry(nodes: nodes),
            policy: ASFWMCPPolicyTelemetry(
                runtimeMode: configuration.mode,
                writesListed: configuration.canListDeveloperWriteTools,
                writeGate: configuration.canListDeveloperWriteTools ? "open" : "testGateMissing"
            )
        )
    }

    func listNodes() async -> [ASFWMCPNodeSummary] {
        listNodesFromBackend()
    }

    func fetchTopology() async -> ASFWMCPTopologySnapshot? {
        guard let topology = backend.mcpTopologySnapshot() else { return nil }
        return ASFWMCPTopologySnapshot(
            generation: topology.generation,
            nodeCount: Int(topology.nodeCount),
            rootNodeId: topology.rootNodeId.map(Int.init),
            irmNodeId: topology.irmNodeId.map(Int.init),
            localNodeId: topology.localNodeId.map(Int.init),
            gapCount: Int(topology.gapCount),
            busBase16: UInt32(topology.busBase16),
            nodes: topology.nodes.map(Self.topologyNode(from:)),
            warnings: topology.warnings
        )
    }

    private static func topologyNode(from node: TopologyNode) -> ASFWMCPTopologyNode {
        ASFWMCPTopologyNode(
            nodeId: Int(node.nodeId),
            portCount: Int(node.portCount),
            gapCount: Int(node.gapCount),
            speedMbps: node.maxSpeedMbps,
            linkActive: node.linkActive,
            contender: node.isIRMCandidate,
            isRoot: node.isRoot,
            initiatedReset: node.initiatedReset,
            parentPort: node.parentPort.map(Int.init),
            ports: node.portStates.enumerated().map { index, state in
                let link = index < node.links.count ? node.links[index] : PortLink.unconnected
                return ASFWMCPTopologyPort(
                    port: index,
                    state: Self.portStateName(state),
                    remoteNodeId: link.connected ? Int(link.remoteNodeId) : nil,
                    remotePort: link.connected ? Int(link.remotePort) : nil
                )
            }
        )
    }

    /// Maps the port state by case. The raw 2-bit values are the Self-ID wire
    /// contract and must never be reordered or derived arithmetically.
    private static func portStateName(_ state: PortState) -> String {
        switch state {
        case .notPresent: return "notPresent"
        case .notActive: return "notActive"
        case .parent: return "parent"
        case .child: return "child"
        }
    }

    func fetchIrmAllocations() async -> ASFWMCPIrmAllocationReport? {
        guard let local = backend.mcpLocalIrmResourceSnapshot() else { return nil }
        return ASFWMCPIrmAllocationReport(
            generation: local.generation,
            irmNodeId: local.irmNodeId,
            localIsIRM: local.isLocalIRM,
            readbackValid: local.readbackValid,
            bandwidthAvailable: local.bandwidthAvailable,
            channelsAvailable31_0: local.channelsAvailable31_0,
            channelsAvailable63_32: local.channelsAvailable63_32
        )
    }

    func fetchOhciSnapshot() async -> ASFWMCPOhciSnapshot? {
        guard let diagnostics = try? backend.mcpFetchDiagnostics() else { return nil }
        let ohci = diagnostics.ohci
        return ASFWMCPOhciSnapshot(
            generation: diagnostics.busContract.header.generation,
            registers: ASFWMCPOhciRegisterMap.covered.map { entry in
                ASFWMCPOhciRegister(name: entry.name, offset: entry.offset, value: ohci[keyPath: entry.field])
            }
        )
    }

    func fetchConfigROM(deviceID: DeviceInstanceID,
                        generation: UInt32) async -> Result<ASFWMCPConfigRomSummary, ASFWMCPConfigRomLookupFailure> {
        guard generation <= UInt32(UInt16.max) else {
            return .failure(.generationOutOfRange(generation))
        }
        // This resolves the node *before* the driver is asked for anything, so an
        // empty or unparsable discovery payload lands here — not in the ROM store.
        // Reporting that as "no ROM cached" is what previously sent the diagnosis
        // into the driver; the instance count distinguishes the two on sight.
        let nodes = await listNodes()
        guard let node = nodes.first(where: { $0.deviceInstanceId == deviceID }) else {
            return .failure(.unknownDeviceInstance(instanceCount: nodes.count))
        }
        guard let fetched = backend.mcpConfigROM(
            deviceID: deviceID,
            expectedGeneration: UInt16(truncatingIfNeeded: generation)
        ) else {
            return .failure(.notCached)
        }

        let byteCount = fetched.data.count
        let rawQuadlets = Self.bigEndianQuadlets(from: fetched.data)
        let base = ASFWMCPConfigRomSummary(
            nodeId: node.nodeId,
            requestedGeneration: UInt32(fetched.requestedGeneration),
            resolvedGeneration: UInt32(fetched.resolvedGeneration),
            exactGenerationMatch: fetched.isExactGenerationMatch,
            byteCount: byteCount,
            quadletCount: byteCount / 4,
            rootDirectoryStartQuadlet: nil,
            parsed: false,
            parseNote: nil,
            guid: nil,
            busName: nil,
            irmc: nil, cmc: nil, isc: nil, bmc: nil,
            maxRec: nil, linkSpeed: nil,
            vendorName: nil, modelName: nil,
            vendorId: nil, modelId: nil, modalias: nil,
            units: [],
            diagnostics: [],
            bibFields: [],
            rawQuadlets: rawQuadlets,
            tree: []
        )

        // The discovery cache legitimately holds only a fetched prefix of a ROM, so a
        // parse failure is reported as an unparsed prefix rather than as a malformed
        // device ROM. Callers needing proof must do a generation-pinned full read.
        guard let tree = try? RomParser.parse(data: fetched.data) else {
            return .success(base.withParseNote(
                "Cached bytes could not be parsed as a complete ROM; this is expected when discovery cached only a prefix."
            ))
        }

        let summary = Summarizer.summarize(tree: tree)
        let options = tree.busInfo.busOptions
        return .success(ASFWMCPConfigRomSummary(
            nodeId: node.nodeId,
            requestedGeneration: UInt32(fetched.requestedGeneration),
            resolvedGeneration: UInt32(fetched.resolvedGeneration),
            exactGenerationMatch: fetched.isExactGenerationMatch,
            byteCount: byteCount,
            quadletCount: byteCount / 4,
            rootDirectoryStartQuadlet: tree.rootDirectoryStartQ,
            parsed: true,
            parseNote: nil,
            guid: String(format: "0x%016llX", tree.busInfo.guid),
            busName: tree.busInfo.busNameString,
            irmc: options.irmc,
            cmc: options.cmc,
            isc: options.isc,
            bmc: options.bmc,
            maxRec: options.maxRec,
            linkSpeed: options.linkSpd,
            vendorName: summary.vendorName,
            modelName: summary.modelName,
            vendorId: summary.vendorId,
            modelId: summary.modelId,
            modalias: summary.modalias,
            units: summary.units.map {
                ASFWMCPConfigRomUnit(
                    specifierId: $0.specifierId,
                    version: $0.version,
                    modelId: $0.modelId,
                    modelName: $0.modelName
                )
            },
            diagnostics: tree.diagnostics.map { "\($0.severity.rawValue): \($0.message)" },
            bibFields: ASFWMCPConfigRomBIBField.makeFields(from: tree.busInfo),
            rawQuadlets: rawQuadlets,
            tree: tree.rootDirectory.map(ASFWMCPConfigRomTreeEntry.init(entry:))
        ))
    }

    func listAVCUnits() async -> [ASFWMCPAVCUnitSummary] {
        listAVCUnitsFromBackend()
    }

    private func listAVCUnitsFromBackend() -> [ASFWMCPAVCUnitSummary] {
        (backend.mcpAVCUnits() ?? []).map { unit in
            ASFWMCPAVCUnitSummary(
                id: unit.id,
                observedGuid: unit.observedGuid,
                generation: unit.generation,
                nodeId: physicalNodeId(unit.nodeID),
                vendorId: unit.vendorID,
                modelId: unit.modelID,
                isoInputPlugCount: unit.isoInputPlugs,
                isoOutputPlugCount: unit.isoOutputPlugs,
                externalInputPlugCount: unit.extInputPlugs,
                externalOutputPlugCount: unit.extOutputPlugs,
                subunits: unit.subunits.map {
                    .init(
                        type: $0.type,
                        id: $0.subunitID,
                        sourcePlugCount: $0.numSrcPlugs,
                        destinationPlugCount: $0.numDestPlugs
                    )
                }
            )
        }
    }

    func avcSubunitCapabilities(
        unitID: UnitInstanceID,
        type: UInt8,
        id: UInt8
    ) async -> ASFWMCPAVCSubunitCapabilities? {
        guard let capabilities = backend.mcpAVCSubunitCapabilities(unitID: unitID, type: type, id: id) else {
            return nil
        }
        return ASFWMCPAVCSubunitCapabilities(
            hasAudio: capabilities.hasAudioCapability,
            hasMIDI: capabilities.hasMidiCapability,
            hasSMPTE: capabilities.hasSmpteCapability,
            currentRateCode: capabilities.currentRate,
            supportedRatesMask: capabilities.supportedRatesMask,
            plugs: capabilities.plugs.map { plug in
                .init(
                    id: plug.plugID,
                    isInput: plug.isInput,
                    type: plug.type,
                    name: plug.name,
                    signalBlocks: plug.signalBlocks.map {
                        .init(formatCode: $0.formatCode, channelCount: $0.channelCount)
                    },
                    supportedFormats: plug.supportedFormats.map {
                        .init(
                            sampleRateCode: $0.sampleRateCode,
                            formatCode: $0.formatCode,
                            channelCount: $0.channelCount
                        )
                    }
                )
            }
        )
    }

    func listRecentTransactions(limit: Int) async -> [ASFWMCPTransactionEvent] {
        guard let diagnostics = try? backend.mcpFetchDiagnostics() else { return [] }
        return recentTransactions(from: diagnostics.asyncTrace, limit: limit)
    }

    func executeReadQuadlet(_ request: ASFWMCPReadQuadletRequest) async -> ASFWMCPTransactionResult {
        await executeTransaction(
            kind: request.kind,
            address: request.address,
            payloadCapacity: 4,
            issue: {
                backend.mcpAsyncRead(
                    deviceID: request.address.deviceInstanceId,
                    addressHigh: request.address.addressHigh,
                    addressLow: request.address.addressLow,
                    length: 4
                )
            }
        )
    }

    func executeReadBlock(_ request: ASFWMCPReadBlockRequest) async -> ASFWMCPTransactionResult {
        if request.validationError != nil {
            return .malformed(kind: request.kind, correlationId: correlationId(request.kind), generation: request.address.generation)
        }

        return await executeTransaction(
            kind: request.kind,
            address: request.address,
            payloadCapacity: Int(request.length),
            issue: {
                backend.mcpAsyncBlockRead(
                    deviceID: request.address.deviceInstanceId,
                    addressHigh: request.address.addressHigh,
                    addressLow: request.address.addressLow,
                    length: request.length
                )
            }
        )
    }

    func executeWriteQuadlet(_ request: ASFWMCPWriteQuadletRequest) async -> ASFWMCPTransactionResult {
        let writeResult = await executeTransaction(
            kind: request.kind,
            address: request.address,
            payloadCapacity: 4,
            issue: {
                backend.mcpAsyncWrite(
                    deviceID: request.address.deviceInstanceId,
                    addressHigh: request.address.addressHigh,
                    addressLow: request.address.addressLow,
                    payload: Data(quadletBytes(request.value))
                )
            }
        )

        guard request.verifyReadback, writeResult.ok else { return writeResult }
        let readback = await executeReadQuadlet(ASFWMCPReadQuadletRequest(address: request.address))
        return writeResult.replacingVerificationPayload(readback.payload, ok: readback.ok)
    }

    func executeWriteBlock(_ request: ASFWMCPWriteBlockRequest) async -> ASFWMCPTransactionResult {
        if request.validationError != nil {
            return .malformed(kind: request.kind, correlationId: correlationId(request.kind), generation: request.address.generation)
        }

        let writeResult = await executeTransaction(
            kind: request.kind,
            address: request.address,
            payloadCapacity: request.payload.count,
            issue: {
                backend.mcpAsyncBlockWrite(
                    deviceID: request.address.deviceInstanceId,
                    addressHigh: request.address.addressHigh,
                    addressLow: request.address.addressLow,
                    payload: Data(request.payload)
                )
            }
        )

        guard request.verifyReadback, writeResult.ok else { return writeResult }
        let readback = await executeReadBlock(ASFWMCPReadBlockRequest(address: request.address, length: UInt32(request.payload.count)))
        return writeResult.replacingVerificationPayload(readback.payload, ok: readback.ok)
    }

    func executeCompareSwap(_ request: ASFWMCPCompareSwapRequest) async -> ASFWMCPTransactionResult {
        await executeTransaction(
            kind: request.kind,
            address: request.address,
            payloadCapacity: 4,
            issue: {
                backend.mcpAsyncCompareSwap(
                    deviceID: request.address.deviceInstanceId,
                    addressHigh: request.address.addressHigh,
                    addressLow: request.address.addressLow,
                    compareValue: Data(quadletBytes(request.expected)),
                    newValue: Data(quadletBytes(request.swap))
                )
            }
        )
    }

    func recentFcpRecords(limit: Int) async -> [ASFWMCPFcpRecord] {
        guard limit > 0 else { return [] }
        return Array(fcpRecords.suffix(limit).reversed())
    }

    /// Retains the exchange and returns the receipt unchanged so call sites stay
    /// single-expression returns.
    @discardableResult
    private func recordFcp(
        _ request: ASFWMCPFcpCommandRequest,
        _ receipt: ASFWMCPFcpCommandReceipt
    ) -> ASFWMCPFcpCommandReceipt {
        fcpRecords.append(
            ASFWMCPFcpRecord(
                correlationId: receipt.correlationId,
                targetUnitID: receipt.targetUnitID,
                nodeId: receipt.observedNodeId,
                generation: receipt.observedGeneration,
                intent: request.intent.rawValue,
                request: request.payload,
                response: receipt.response,
                status: receipt.status.rawValue,
                durationUsec: receipt.durationUsec,
                capturedAtUptimeNs: DispatchTime.now().uptimeNanoseconds
            )
        )
        if fcpRecords.count > Self.fcpRecordCapacity {
            fcpRecords.removeFirst(fcpRecords.count - Self.fcpRecordCapacity)
        }
        return receipt
    }

    func executeFCPCommand(_ request: ASFWMCPFcpCommandRequest) async -> ASFWMCPFcpCommandReceipt {
        let correlationId = "live-fcp-\(UUID().uuidString)"
        let currentGeneration = backend.mcpCurrentGeneration() ?? 0
        guard backend.mcpIsConnected else {
            return recordFcp(request, fcpReceipt(request, observedNodeId: nil, observedGeneration: currentGeneration,
                              response: nil, status: .unavailable, correlationId: correlationId, durationUsec: nil))
        }
        guard currentGeneration == request.address.generation else {
            return recordFcp(request, fcpReceipt(request, observedNodeId: nil, observedGeneration: currentGeneration,
                              response: nil, status: .staleGeneration, correlationId: correlationId, durationUsec: nil))
        }

        guard let unit = listAVCUnitsFromBackend().first(where: {
            $0.id == request.targetUnitID &&
                $0.nodeId == request.address.nodeId &&
                $0.generation == currentGeneration
        }) else {
            return recordFcp(request, fcpReceipt(request, observedNodeId: nil, observedGeneration: currentGeneration,
                              response: nil, status: .unavailable, correlationId: correlationId, durationUsec: nil))
        }

        let started = Date()
        let response = backend.mcpSendRawFCPCommand(
            unitID: request.targetUnitID,
            frame: Data(request.payload),
            timeoutMs: 15_000
        )
        let completedGeneration = backend.mcpCurrentGeneration() ?? currentGeneration
        let status: ASFWMCPTransactionStatus = completedGeneration == currentGeneration
            ? (response == nil ? .timeout : .ok)
            : .busReset
        return recordFcp(request, fcpReceipt(
            request,
            observedNodeId: unit.nodeId,
            observedGeneration: completedGeneration,
            response: response.map(Array.init),
            status: status,
            correlationId: correlationId,
            durationUsec: elapsedUsec(since: started)
        ))
    }

    func executePhase88Streaming(endpointID: AudioEndpointID, start: Bool) async -> ASFWMCPPhase88StreamingReceipt {
        guard backend.mcpIsConnected else {
            return ASFWMCPPhase88StreamingReceipt(endpointId: endpointID, started: start, status: -536_870_201)
        }
        return ASFWMCPPhase88StreamingReceipt(
            endpointId: endpointID,
            started: start,
            status: backend.mcpSetAudioStreaming(endpointID: endpointID, enabled: start)
        )
    }

    func executeBusReset(_ request: ASFWMCPBusResetRequest) async -> ASFWMCPBusResetReceipt {
        let correlationId = "live-bus-reset-\(UUID().uuidString)"
        let currentGeneration = backend.mcpCurrentGeneration() ?? 0
        guard backend.mcpIsConnected else {
            return busResetReceipt(request, acceptedGeneration: nil, observedGeneration: currentGeneration,
                                   status: .unavailable, correlationId: correlationId, durationUsec: nil)
        }
        guard currentGeneration == request.generation else {
            return busResetReceipt(request, acceptedGeneration: nil, observedGeneration: currentGeneration,
                                   status: .staleGeneration, correlationId: correlationId, durationUsec: nil)
        }

        let started = Date()
        guard let acceptedGeneration = backend.mcpRequestUserBusReset(
            expectedGeneration: request.generation,
            shortReset: request.shortReset
        ) else {
            return busResetReceipt(request, acceptedGeneration: nil, observedGeneration: currentGeneration,
                                   status: .unavailable, correlationId: correlationId,
                                   durationUsec: elapsedUsec(since: started))
        }

        let deadline = started.addingTimeInterval(busResetTimeout)
        while Date() < deadline {
            guard backend.mcpIsConnected else {
                return busResetReceipt(request, acceptedGeneration: acceptedGeneration,
                                       observedGeneration: backend.mcpCurrentGeneration() ?? currentGeneration,
                                       status: .unavailable, correlationId: correlationId,
                                       durationUsec: elapsedUsec(since: started))
            }
            if let observedGeneration = backend.mcpCurrentGeneration(), observedGeneration != currentGeneration {
                return busResetReceipt(request, acceptedGeneration: acceptedGeneration,
                                       observedGeneration: observedGeneration, status: .ok,
                                       correlationId: correlationId, durationUsec: elapsedUsec(since: started))
            }
            try? await Task.sleep(nanoseconds: pollIntervalNs)
        }

        return busResetReceipt(request, acceptedGeneration: acceptedGeneration,
                               observedGeneration: backend.mcpCurrentGeneration() ?? currentGeneration,
                               status: .timeout, correlationId: correlationId,
                               durationUsec: elapsedUsec(since: started))
    }

    func executeIRMSnapshot(_ request: ASFWMCPIrmSnapshotRequest) async -> ASFWMCPIrmResourceSnapshot {
        let correlationId = "live-irm-snapshot-\(UUID().uuidString)"
        let currentGeneration = backend.mcpCurrentGeneration() ?? 0
        guard backend.mcpIsConnected else {
            return irmSnapshot(
                request, observedGeneration: currentGeneration, irmNodeId: nil,
                bandwidthAvailable: nil, channelsAvailable31_0: nil, channelsAvailable63_32: nil,
                status: .unavailable, correlationId: correlationId, durationUsec: nil
            )
        }
        guard request.generation == currentGeneration else {
            return irmSnapshot(
                request, observedGeneration: currentGeneration, irmNodeId: nil,
                bandwidthAvailable: nil, channelsAvailable31_0: nil, channelsAvailable63_32: nil,
                status: .staleGeneration, correlationId: correlationId, durationUsec: nil
            )
        }

        // A local IRM owns these CSRs through OHCI CSRControl. Reading them as
        // an async transaction addressed to ourselves cannot yield a remote
        // AR response, so consume the driver's generation-consistent local
        // resource snapshot instead. This follows the local-CSR split owned
        // by LocalIRMResourceController and cross-checked with Linux
        // firewire-ohci's local CSRControl path.
        let started = Date()
        if let local = backend.mcpLocalIrmResourceSnapshot(),
           local.localNodeId == local.irmNodeId {
            guard local.generation == request.generation else {
                return irmSnapshot(
                    request, observedGeneration: currentGeneration, irmNodeId: local.irmNodeId,
                    bandwidthAvailable: nil, channelsAvailable31_0: nil, channelsAvailable63_32: nil,
                    status: .unavailable, correlationId: correlationId, durationUsec: nil
                )
            }
            guard local.isLocalIRM, local.readbackValid else {
                return irmSnapshot(
                    request, observedGeneration: currentGeneration, irmNodeId: local.irmNodeId,
                    bandwidthAvailable: nil, channelsAvailable31_0: nil, channelsAvailable63_32: nil,
                    status: .unavailable, correlationId: correlationId, durationUsec: nil
                )
            }
            return irmSnapshot(
                request,
                observedGeneration: currentGeneration,
                irmNodeId: local.irmNodeId,
                bandwidthAvailable: local.bandwidthAvailable,
                channelsAvailable31_0: local.channelsAvailable31_0,
                channelsAvailable63_32: local.channelsAvailable63_32,
                status: .ok,
                correlationId: correlationId,
                durationUsec: elapsedUsec(since: started)
            )
        }

        guard let controllerStatus = backend.mcpControllerStatus(),
              let irmNodeId = controllerStatus.irmNodeID.map({ UInt32($0 & 0x003F) }) else {
            return irmSnapshot(
                request, observedGeneration: currentGeneration, irmNodeId: nil,
                bandwidthAvailable: nil, channelsAvailable31_0: nil, channelsAvailable63_32: nil,
                status: .unavailable, correlationId: correlationId, durationUsec: nil
            )
        }
        if irmNodeId == controllerStatus.localNodeID.map({ UInt32($0 & 0x003F) }) {
            return irmSnapshot(
                request, observedGeneration: currentGeneration, irmNodeId: irmNodeId,
                bandwidthAvailable: nil, channelsAvailable31_0: nil, channelsAvailable63_32: nil,
                status: .unavailable, correlationId: correlationId, durationUsec: nil
            )
        }

        // Keep the established IRMClient order: bandwidth, channels 31...0,
        // then channels 63...32. Each CSR is independently read-only.
        let addresses: [UInt32] = [0xF000_0220, 0xF000_0224, 0xF000_0228]
        guard let irmDeviceID = backend.mcpDiscoveredDevices()?.first(where: {
            UInt32($0.nodeId) == irmNodeId && $0.generation == request.generation &&
                !$0.isQuarantined
        })?.id else {
            return irmSnapshot(
                request, observedGeneration: currentGeneration, irmNodeId: irmNodeId,
                bandwidthAvailable: nil, channelsAvailable31_0: nil,
                channelsAvailable63_32: nil, status: .unavailable,
                correlationId: correlationId, durationUsec: nil
            )
        }
        var values: [UInt32] = []
        for addressLow in addresses {
            let result = await executeReadQuadlet(
                ASFWMCPReadQuadletRequest(
                    address: ASFWMCPAddress(
                        deviceInstanceId: irmDeviceID,
                        nodeId: irmNodeId,
                        generation: request.generation,
                        addressHigh: 0xFFFF,
                        addressLow: addressLow
                    )
                )
            )
            guard result.ok, let value = quadletValue(result.payload) else {
                return irmSnapshot(
                    request,
                    observedGeneration: backend.mcpCurrentGeneration() ?? currentGeneration,
                    irmNodeId: irmNodeId,
                    bandwidthAvailable: values[safe: 0],
                    channelsAvailable31_0: values[safe: 1],
                    channelsAvailable63_32: nil,
                    status: result.status,
                    correlationId: correlationId,
                    durationUsec: elapsedUsec(since: started)
                )
            }
            values.append(value)
        }

        let observedGeneration = backend.mcpCurrentGeneration() ?? currentGeneration
        return irmSnapshot(
            request,
            observedGeneration: observedGeneration,
            irmNodeId: irmNodeId,
            bandwidthAvailable: values[0],
            channelsAvailable31_0: values[1],
            channelsAvailable63_32: values[2],
            status: observedGeneration == request.generation ? .ok : .busReset,
            correlationId: correlationId,
            durationUsec: elapsedUsec(since: started)
        )
    }

    func queryLogRecords(_ query: ASFWLogRingQuery) async -> ASFWLogRingQueryResponse? {
        guard backend.mcpIsConnected else { return nil }
        return backend.mcpQueryLogRecords(query)
    }

    func logRingStats() async -> ASFWLogRingStats? {
        guard backend.mcpIsConnected else { return nil }
        return backend.mcpLogRingStats()
    }

    func fetchAudioStreamHealth() async -> [ASFWMCPAudioStreamHealth] {
        guard backend.mcpIsConnected else { return [] }
        guard let snapshot = backend.mcpAudioTelemetry() else { return [] }
        return snapshot.endpoints.map { $0.mcpStreamHealth }
    }

    func fetchAudioCursors() async -> [ASFWMCPAudioCursorSnapshot] {
        guard backend.mcpIsConnected else { return [] }
        guard let snapshot = backend.mcpAudioTelemetry() else { return [] }
        return snapshot.endpoints.map { $0.mcpAudioCursors }
    }

    private func executeTransaction(
        kind: ASFWMCPTransactionKind,
        address: ASFWMCPAddress,
        payloadCapacity: Int,
        issue: () -> UInt16?
    ) async -> ASFWMCPTransactionResult {
        let correlationId = correlationId(kind)
        guard backend.mcpIsConnected else {
            return unavailable(kind: kind, generation: address.generation, correlationId: correlationId, reason: "Driver is not connected.")
        }

        guard let currentGeneration = backend.mcpCurrentGeneration() else {
            return unavailable(kind: kind, generation: address.generation, correlationId: correlationId, reason: "Current bus generation is unavailable.")
        }

        guard currentGeneration == address.generation else {
            return ASFWMCPTransactionResult(
                kind: kind,
                ok: false,
                status: .staleGeneration,
                generation: currentGeneration,
                correlationId: correlationId,
                rCode: "staleGeneration"
            )
        }

        let started = Date()
        guard let handle = issue() else {
            return unavailable(
                kind: kind,
                generation: currentGeneration,
                correlationId: correlationId,
                reason: backend.mcpLastError ?? "Driver did not return a transaction handle."
            )
        }

        let deadline = started.addingTimeInterval(transactionTimeout)
        while Date() < deadline {
            if let result = backend.mcpTransactionResult(handle: handle, initialPayloadCapacity: max(payloadCapacity, 64)) {
                return mapResult(
                    result,
                    kind: kind,
                    generation: currentGeneration,
                    correlationId: correlationId,
                    started: started
                )
            }

            try? await Task.sleep(nanoseconds: pollIntervalNs)
        }

        return ASFWMCPTransactionResult(
            kind: kind,
            ok: false,
            status: .timeout,
            generation: currentGeneration,
            correlationId: correlationId,
            rCode: "timeout",
            durationUsec: elapsedUsec(since: started)
        )
    }

    private func mapResult(
        _ result: ASFWDriverConnector.AsyncTransactionResult,
        kind: ASFWMCPTransactionKind,
        generation: UInt32,
        correlationId: String,
        started: Date
    ) -> ASFWMCPTransactionResult {
        let status = transactionStatus(asyncStatus: result.status, rCode: result.responseCode)
        return ASFWMCPTransactionResult(
            kind: kind,
            ok: status == .ok,
            status: status,
            generation: generation,
            correlationId: correlationId,
            rCode: rCodeName(result.responseCode),
            durationUsec: elapsedUsec(since: started),
            payload: result.payload.isEmpty ? nil : Array(result.payload)
        )
    }

    private func listNodesFromBackend() -> [ASFWMCPNodeSummary] {
        let devices = backend.mcpDiscoveredDevices() ?? []
        let avcNodeIds = Set((backend.mcpAVCUnits() ?? []).map { physicalNodeId($0.nodeID) })
        let busBase16 = (try? backend.mcpFetchDiagnostics()).map { UInt16(truncatingIfNeeded: $0.topology.busBase16) } ?? 0

        return devices.map { device in
            let physicalNode = UInt32(device.nodeId)
            let address16 = busBase16 | UInt16(truncatingIfNeeded: physicalNode & 0x3F)
            let hints = protocolHints(for: device, avcNodeIds: avcNodeIds)
            return ASFWMCPNodeSummary(
                deviceInstanceId: device.id,
                nodeId: physicalNode,
                address16: String(format: "0x%04X", address16),
                observedGuid: device.observedGuidHex,
                vendorId: String(format: "0x%06X", device.vendorId),
                modelId: String(format: "0x%06X", device.modelId),
                vendorName: device.vendorName.isEmpty ? nil : device.vendorName,
                modelName: device.modelName.isEmpty ? nil : device.modelName,
                configRomCached: true,
                protocolHints: hints
            )
        }
    }

    private func physicalNodeId(_ nodeId: UInt16) -> UInt32 {
        UInt32(nodeId & 0x003F)
    }

    private func protocolHints(for device: FWDeviceInfo, avcNodeIds: Set<UInt32>) -> [String] {
        var hints = Set<String>()
        // Exact Config-ROM identity, so BeBoB diagnostics remain visible even
        // when a BridgeCo unit ignores generic AV/C UNIT_INFO.
        if device.vendorId == 0x000AAC && device.modelId == 0x000003 {
            hints.insert("bebob")
            hints.insert("cmp")
        }
        if avcNodeIds.contains(UInt32(device.nodeId)) {
            hints.insert("avc")
            hints.insert("cmp")
        }
        if device.hasSBP2Unit {
            hints.insert("sbp2")
        }

        let vendor = device.vendorName.lowercased()
        let model = device.modelName.lowercased()
        if device.vendorId == 0x00130E || vendor.contains("tcat") || model.contains("dice") || model.contains("tcat") {
            hints.insert("dice_tcat")
        }

        return hints.sorted()
    }

    private func protocolTelemetry(nodes: [ASFWMCPNodeSummary]) -> ASFWMCPProtocolTelemetry {
        ASFWMCPProtocolTelemetry(
            avcUnits: UInt32(nodes.filter { $0.protocolHints.contains("avc") }.count),
            sbp2Units: UInt32(nodes.filter { $0.protocolHints.contains("sbp2") }.count),
            diceTcatNodes: UInt32(nodes.filter { $0.protocolHints.contains("dice_tcat") }.count),
            cmpCapableNodes: UInt32(nodes.filter { $0.protocolHints.contains("cmp") }.count)
        )
    }

    private func recentTransactions(from trace: ASFWDiagAsyncTrace?, limit: Int) -> [ASFWMCPTransactionEvent] {
        guard let trace else { return [] }
        let eventCount = Int(min(trace.eventCount, UInt32(ASFW_DIAG_MAX_ASYNC_EVENTS)))
        let clampedLimit = max(0, min(limit, eventCount))
        let events: [ASFWDiagAsyncEvent] = withUnsafeBytes(of: trace.events) { buffer in
            Array(buffer.bindMemory(to: ASFWDiagAsyncEvent.self).prefix(eventCount))
        }
        return events.suffix(clampedLimit).map { event in
            ASFWMCPTransactionEvent(
                timestampNs: event.timestampNs,
                generation: event.generation,
                direction: event.direction == 1 ? "tx" : "rx",
                context: contextName(direction: event.direction, context: event.context),
                tLabel: event.tLabel,
                tCode: tCodeName(event.tCode),
                sourceId: String(format: "0x%04X", event.sourceId),
                destinationId: String(format: "0x%04X", event.destinationId),
                address: String(format: "0x%012llX", event.address),
                payloadBytes: event.payloadBytes,
                ackCode: ackCodeName(event.ackCode),
                rCode: rCodeName(UInt8(truncatingIfNeeded: event.rCode)),
                speed: speedName(event.speed),
                matchedTransaction: event.matchedTransaction != 0,
                dropReason: dropReasonName(event.dropReason)
            )
        }
    }

    private func transactionStatus(asyncStatus: UInt32, rCode: UInt8) -> ASFWMCPTransactionStatus {
        switch asyncStatus {
        case 0 where rCode == 0:
            return .ok
        case 0:
            return .rcodeError
        case 1:
            return .timeout
        case 4:
            return .busReset
        case 6:
            return .compareFailed
        case 7:
            return .staleGeneration
        default:
            return .rcodeError
        }
    }

    private func unavailable(
        kind: ASFWMCPTransactionKind,
        generation: UInt32,
        correlationId: String,
        reason: String
    ) -> ASFWMCPTransactionResult {
        ASFWMCPTransactionResult(
            kind: kind,
            ok: false,
            status: .unavailable,
            generation: generation,
            correlationId: correlationId,
            rCode: reason
        )
    }

    private func fcpReceipt(
        _ request: ASFWMCPFcpCommandRequest,
        observedNodeId: UInt32?,
        observedGeneration: UInt32,
        response: [UInt8]?,
        status: ASFWMCPTransactionStatus,
        correlationId: String,
        durationUsec: UInt64?
    ) -> ASFWMCPFcpCommandReceipt {
        ASFWMCPFcpCommandReceipt(
            targetUnitID: request.targetUnitID,
            expectedNodeId: request.address.nodeId,
            expectedGeneration: request.address.generation,
            observedNodeId: observedNodeId,
            observedGeneration: observedGeneration,
            response: response,
            status: status,
            correlationId: correlationId,
            durationUsec: durationUsec,
            policy: nil
        )
    }

    private func busResetReceipt(
        _ request: ASFWMCPBusResetRequest,
        acceptedGeneration: UInt32?,
        observedGeneration: UInt32,
        status: ASFWMCPTransactionStatus,
        correlationId: String,
        durationUsec: UInt64?
    ) -> ASFWMCPBusResetReceipt {
        ASFWMCPBusResetReceipt(
            requestedGeneration: request.generation,
            acceptedGeneration: acceptedGeneration,
            observedGeneration: observedGeneration,
            shortReset: request.shortReset,
            status: status,
            correlationId: correlationId,
            durationUsec: durationUsec,
            policy: nil
        )
    }

    private func irmSnapshot(
        _ request: ASFWMCPIrmSnapshotRequest,
        observedGeneration: UInt32,
        irmNodeId: UInt32?,
        bandwidthAvailable: UInt32?,
        channelsAvailable31_0: UInt32?,
        channelsAvailable63_32: UInt32?,
        status: ASFWMCPTransactionStatus,
        correlationId: String,
        durationUsec: UInt64?
    ) -> ASFWMCPIrmResourceSnapshot {
        ASFWMCPIrmResourceSnapshot(
            requestedGeneration: request.generation,
            observedGeneration: observedGeneration,
            irmNodeId: irmNodeId,
            bandwidthAvailable: bandwidthAvailable,
            channelsAvailable31_0: channelsAvailable31_0,
            channelsAvailable63_32: channelsAvailable63_32,
            status: status,
            correlationId: correlationId,
            durationUsec: durationUsec
        )
    }

    private func correlationId(_ kind: ASFWMCPTransactionKind) -> String {
        "live-\(kind.rawValue)-\(UUID().uuidString)"
    }

    private func elapsedUsec(since started: Date) -> UInt64 {
        UInt64(max(0, Date().timeIntervalSince(started) * 1_000_000))
    }

    private func quadletBytes(_ value: UInt32) -> [UInt8] {
        [
            UInt8((value >> 24) & 0xFF),
            UInt8((value >> 16) & 0xFF),
            UInt8((value >> 8) & 0xFF),
            UInt8(value & 0xFF)
        ]
    }

    private func quadletValue(_ payload: [UInt8]?) -> UInt32? {
        guard let payload, payload.count == 4 else { return nil }
        return (UInt32(payload[0]) << 24) |
            (UInt32(payload[1]) << 16) |
            (UInt32(payload[2]) << 8) |
            UInt32(payload[3])
    }
}

private extension Array {
    subscript(safe index: Int) -> Element? {
        indices.contains(index) ? self[index] : nil
    }
}

private extension UInt32 {
    var nodeIdOrNil: UInt32? {
        self >= 0x3F ? nil : self
    }
}

private extension ASFWMCPTransactionResult {
    func replacingVerificationPayload(_ payload: [UInt8]?, ok: Bool) -> ASFWMCPTransactionResult {
        ASFWMCPTransactionResult(
            kind: kind,
            ok: ok,
            status: ok ? status : .rcodeError,
            generation: generation,
            correlationId: correlationId,
            rCode: rCode,
            durationUsec: durationUsec,
            payload: payload,
            decoded: decoded,
            policy: policy
        )
    }
}

private func ackCodeName(_ code: UInt32) -> String {
    if code == 0xFF { return "-" }
    switch code {
    case 0x01: return "complete"
    case 0x02: return "pending"
    case 0x04: return "busyX"
    case 0x05: return "busyA"
    case 0x06: return "busyB"
    case 0x0D: return "dataError"
    case 0x0E: return "typeError"
    default: return String(format: "0x%02X", code)
    }
}

private func rCodeName(_ code: UInt8) -> String {
    if code == 0xFF { return "-" }
    switch code {
    case 0: return "complete"
    case 4: return "conflictError"
    case 5: return "dataError"
    case 6: return "typeError"
    case 7: return "addressError"
    default: return String(format: "0x%02X", code)
    }
}

private func tCodeName(_ code: UInt32) -> String {
    switch code {
    case 0: return "writeQuadlet"
    case 1: return "writeBlock"
    case 2: return "writeResponse"
    case 4: return "readQuadlet"
    case 5: return "readBlock"
    case 6: return "readQuadletResponse"
    case 7: return "readBlockResponse"
    case 9: return "lock"
    case 11: return "lockResponse"
    default: return String(format: "0x%02X", code)
    }
}

private func speedName(_ speed: UInt32) -> String {
    switch speed {
    case ASFWDiagSpeedS100.rawValue: return "S100"
    case ASFWDiagSpeedS200.rawValue: return "S200"
    case ASFWDiagSpeedS400.rawValue: return "S400"
    case ASFWDiagSpeedS800.rawValue: return "S800"
    case ASFWDiagSpeedS1600.rawValue: return "S1600"
    case ASFWDiagSpeedS3200.rawValue: return "S3200"
    default: return "unknown"
    }
}

private func contextName(direction: UInt32, context: UInt32) -> String {
    let prefix = direction == 1 ? "AT" : "AR"
    let suffix = context == 0 ? "Request" : (context == 1 ? "Response" : "Unknown")
    return "\(prefix)\(suffix)"
}

private func dropReasonName(_ reason: UInt32) -> String? {
    guard reason != 0 else { return nil }
    switch reason {
    case 1: return "ringFull"
    case 2: return "malformed"
    case 3: return "unmatched"
    default: return String(format: "0x%02X", reason)
    }
}
