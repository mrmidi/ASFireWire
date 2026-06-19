import Foundation

@MainActor
protocol ASFWLiveDriverBackend: AnyObject {
    var mcpIsConnected: Bool { get }
    var mcpLastError: String? { get }

    func mcpCurrentGeneration() -> UInt32?
    func mcpControllerStatus() -> ControllerStatus?
    func mcpFetchDiagnostics() throws -> ASFWDiagnosticsSnapshot
    func mcpDiscoveredDevices() -> [FWDeviceInfo]?
    func mcpAVCUnits() -> [AVCUnitInfo]?

    func mcpAsyncRead(destinationID: UInt16, addressHigh: UInt16, addressLow: UInt32, length: UInt32) -> UInt16?
    func mcpAsyncWrite(destinationID: UInt16, addressHigh: UInt16, addressLow: UInt32, payload: Data) -> UInt16?
    func mcpAsyncBlockRead(destinationID: UInt16, addressHigh: UInt16, addressLow: UInt32, length: UInt32) -> UInt16?
    func mcpAsyncBlockWrite(destinationID: UInt16, addressHigh: UInt16, addressLow: UInt32, payload: Data) -> UInt16?
    func mcpAsyncCompareSwap(destinationID: UInt16, addressHigh: UInt16, addressLow: UInt32, compareValue: Data, newValue: Data) -> UInt16?
    func mcpTransactionResult(handle: UInt16, initialPayloadCapacity: Int) -> ASFWDriverConnector.AsyncTransactionResult?
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

    func mcpDiscoveredDevices() -> [FWDeviceInfo]? {
        getDiscoveredDevices()
    }

    func mcpAVCUnits() -> [AVCUnitInfo]? {
        getAVCUnits()
    }

    func mcpAsyncRead(destinationID: UInt16, addressHigh: UInt16, addressLow: UInt32, length: UInt32) -> UInt16? {
        asyncRead(destinationID: destinationID, addressHigh: addressHigh, addressLow: addressLow, length: length)
    }

    func mcpAsyncWrite(destinationID: UInt16, addressHigh: UInt16, addressLow: UInt32, payload: Data) -> UInt16? {
        asyncWrite(destinationID: destinationID, addressHigh: addressHigh, addressLow: addressLow, payload: payload)
    }

    func mcpAsyncBlockRead(destinationID: UInt16, addressHigh: UInt16, addressLow: UInt32, length: UInt32) -> UInt16? {
        asyncBlockRead(destinationID: destinationID, addressHigh: addressHigh, addressLow: addressLow, length: length)
    }

    func mcpAsyncBlockWrite(destinationID: UInt16, addressHigh: UInt16, addressLow: UInt32, payload: Data) -> UInt16? {
        asyncBlockWrite(destinationID: destinationID, addressHigh: addressHigh, addressLow: addressLow, payload: payload)
    }

    func mcpAsyncCompareSwap(destinationID: UInt16, addressHigh: UInt16, addressLow: UInt32, compareValue: Data, newValue: Data) -> UInt16? {
        asyncCompareSwap(
            destinationID: destinationID,
            addressHigh: addressHigh,
            addressLow: addressLow,
            compareValue: compareValue,
            newValue: newValue
        )?.handle
    }

    func mcpTransactionResult(handle: UInt16, initialPayloadCapacity: Int) -> ASFWDriverConnector.AsyncTransactionResult? {
        getTransactionResult(handle: handle, initialPayloadCapacity: initialPayloadCapacity)
    }
}

@MainActor
final class LiveASFWDriverControl: ASFWDriverControlling {
    private let backend: any ASFWLiveDriverBackend
    private let transactionTimeout: TimeInterval
    private let pollIntervalNs: UInt64

    init(
        backend: any ASFWLiveDriverBackend,
        transactionTimeout: TimeInterval = 2.0,
        pollIntervalNs: UInt64 = 25_000_000
    ) {
        self.backend = backend
        self.transactionTimeout = transactionTimeout
        self.pollIntervalNs = pollIntervalNs
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
                    destinationID: UInt16(truncatingIfNeeded: request.address.nodeId),
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
                    destinationID: UInt16(truncatingIfNeeded: request.address.nodeId),
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
                    destinationID: UInt16(truncatingIfNeeded: request.address.nodeId),
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
                    destinationID: UInt16(truncatingIfNeeded: request.address.nodeId),
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
                    destinationID: UInt16(truncatingIfNeeded: request.address.nodeId),
                    addressHigh: request.address.addressHigh,
                    addressLow: request.address.addressLow,
                    compareValue: Data(quadletBytes(request.expected)),
                    newValue: Data(quadletBytes(request.swap))
                )
            }
        )
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
                nodeId: physicalNode,
                address16: String(format: "0x%04X", address16),
                guid: String(format: "0x%016llX", device.guid),
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
