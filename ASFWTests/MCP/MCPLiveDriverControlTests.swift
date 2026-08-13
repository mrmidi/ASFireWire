import Foundation
import Testing
@testable import ASFW

@MainActor
struct MCPLiveDriverControlTests {
    @Test func readQuadletPollsBackendAndMapsPayload() async {
        let backend = FakeLiveDriverBackend()
        backend.results[0x44] = ASFWDriverConnector.AsyncTransactionResult(
            status: 0,
            dataLength: 4,
            responseCode: 0,
            payload: Data([0x31, 0x33, 0x39, 0x34])
        )
        let control = LiveASFWDriverControl(backend: backend, transactionTimeout: 0.1, pollIntervalNs: 1_000)

        let result = await control.executeReadQuadlet(
            ASFWMCPReadQuadletRequest(address: address(generation: 17))
        )

        #expect(result.ok == true)
        #expect(result.status == .ok)
        #expect(result.rCode == "complete")
        #expect(result.payload == [0x31, 0x33, 0x39, 0x34])
        #expect(backend.reads == 1)
        #expect(backend.resultPolls == 1)
    }

    @Test func staleGenerationDoesNotReachDriverTransactionPath() async {
        let backend = FakeLiveDriverBackend()
        backend.generation = 18
        let control = LiveASFWDriverControl(backend: backend, transactionTimeout: 0.1, pollIntervalNs: 1_000)

        let result = await control.executeReadBlock(
            ASFWMCPReadBlockRequest(address: address(generation: 17), length: 4)
        )

        #expect(result.ok == false)
        #expect(result.status == .staleGeneration)
        #expect(result.rCode == "staleGeneration")
        #expect(backend.blockReads == 0)
        #expect(backend.resultPolls == 0)
    }

    @Test func nodeDiscoveryMapsProtocolHints() async {
        let backend = FakeLiveDriverBackend()
        backend.devices = [
            device(
                id: 1,
                observedGuid: 0x0011_2233_4455_6677,
                vendorId: 0x0003DB,
                modelId: 0x000001,
                vendorName: "Apogee",
                modelName: "Duet",
                nodeId: 0,
                generation: 17,
                state: .ready,
                units: [],
                deviceKind: 0
            ),
            device(
                id: 2,
                observedGuid: 0x00AA_BBCC_DDEE_FF00,
                vendorId: 0x00130E,
                modelId: 0x000002,
                vendorName: "TCAT",
                modelName: "DICE",
                nodeId: 1,
                generation: 17,
                state: .ready,
                units: [sbp2Unit()],
                deviceKind: 4
            )
        ]
        backend.avcUnits = [
            avcUnit(
                deviceID: DeviceInstanceID(1),
                observedGuid: 0x0011_2233_4455_6677,
                nodeID: 0,
                vendorID: 0x0003DB,
                modelID: 0x000001,
                subunits: [],
                isoInputPlugs: 1,
                isoOutputPlugs: 1,
                extInputPlugs: 1,
                extOutputPlugs: 1
            )
        ]

        let nodes = await LiveASFWDriverControl(backend: backend).listNodes()

        let duet = nodes.first { $0.nodeId == 0 }
        let dice = nodes.first { $0.nodeId == 1 }
        #expect(duet?.protocolHints == ["avc", "cmp"])
        #expect(dice?.protocolHints == ["dice_tcat", "sbp2"])
    }

    @Test func configRomProjectsAnnotatedBIBTreeAndBoundedRawCache() async throws {
        let backend = FakeLiveDriverBackend()
        backend.devices = [device(
            id: 1,
            observedGuid: 0x0013_0E04_0200_4713,
            vendorId: 0x00130E,
            modelId: 0x000008,
            vendorName: "Focusrite",
            modelName: "Saffire",
            nodeId: 0,
            generation: 17,
            state: .ready,
            units: [],
            deviceKind: 0
        )]
        backend.configROM = ASFWDriverConnector.ConfigROMFetchResult(
            data: configRomFixture(),
            requestedGeneration: 17,
            resolvedGeneration: 17
        )

        let report = try (await LiveASFWDriverControl(backend: backend).fetchConfigROM(
            deviceID: DeviceInstanceID(1), generation: 17)).get()

        #expect(report.parsed)
        #expect(report.rootDirectoryStartQuadlet == 5)
        #expect(report.guid == "0x00130E0402004713")
        #expect(report.vendorId == 0x00130E)
        #expect(report.modelId == 0x000008)
        #expect(report.rawQuadlets == [
            0x0404_0000, 0x3133_3934, 0xE0FF_8112, 0x0013_0E04,
            0x0200_4713, 0x0002_0000, 0x0300_130E, 0x1700_0008
        ])
        #expect(report.bibFields.first?.name == "bus_info_length")
        #expect(report.bibFields.first?.meaning.contains("root directory") == true)
        #expect(report.tree.map(\.key) == ["VENDOR", "MODEL"])
    }

    @Test func configRomDistinguishesAnUnresolvableInstanceFromAnUncachedRom() async throws {
        // An empty discovery payload used to surface as "no Config ROM is cached",
        // which reads as a driver-side ROM-store miss when the driver was never asked.
        let emptyBus = FakeLiveDriverBackend()
        emptyBus.configROM = ASFWDriverConnector.ConfigROMFetchResult(
            data: configRomFixture(), requestedGeneration: 17, resolvedGeneration: 17
        )
        let unresolved = await LiveASFWDriverControl(backend: emptyBus).fetchConfigROM(
            deviceID: DeviceInstanceID(1), generation: 17)
        #expect(unresolved == .failure(.unknownDeviceInstance(instanceCount: 0)))

        // A device that *is* discovered but has no cached ROM is the genuine miss.
        let discovered = FakeLiveDriverBackend()
        discovered.devices = [device(
            id: 1,
            observedGuid: 0x0013_0E04_0200_4713,
            vendorId: 0x00130E,
            modelId: 0x000008,
            vendorName: "Focusrite",
            modelName: "Saffire",
            nodeId: 0,
            generation: 17,
            state: .ready,
            units: [],
            deviceKind: 0
        )]
        discovered.configROM = nil
        let uncached = await LiveASFWDriverControl(backend: discovered).fetchConfigROM(
            deviceID: DeviceInstanceID(1), generation: 17)
        #expect(uncached == .failure(.notCached))

        // A generation the 16-bit wire field cannot carry is a caller error, not either of those.
        let outOfRange = await LiveASFWDriverControl(backend: discovered).fetchConfigROM(
            deviceID: DeviceInstanceID(1), generation: 0x1_0000)
        #expect(outOfRange == .failure(.generationOutOfRange(0x1_0000)))
    }

    @Test func avcInspectionMapsDriverEvidenceWithoutFcpTraffic() async throws {
        let backend = FakeLiveDriverBackend()
        backend.avcUnits = [
            avcUnit(
                deviceID: DeviceInstanceID(1),
                observedGuid: 0x0011_2233_4455_6677,
                nodeID: 0xFFC2,
                vendorID: 0x0003DB,
                modelID: 0x01DDDD,
                subunits: [.init(type: 0x0C, subunitID: 0, numSrcPlugs: 1, numDestPlugs: 2)],
                isoInputPlugs: 2,
                isoOutputPlugs: 1,
                extInputPlugs: 0,
                extOutputPlugs: 0
            )
        ]
        backend.avcSubunitCapabilities = try #require(capabilitiesFixture())
        let control = LiveASFWDriverControl(backend: backend)

        let units = await control.listAVCUnits()
        let capabilities = await control.avcSubunitCapabilities(
            unitID: UnitInstanceID(device: DeviceInstanceID(1), unitDirectoryOffset: 0x28),
            type: 0x0C,
            id: 0
        )

        #expect(units.count == 1)
        #expect(units[0].nodeId == 2)
        #expect(units[0].subunits == [.init(type: 0x0C, id: 0, sourcePlugCount: 1, destinationPlugCount: 2)])
        #expect(capabilities?.hasAudio == true)
        #expect(capabilities?.hasMIDI == true)
        #expect(capabilities?.currentRateCode == 0x04)
        #expect(capabilities?.plugs == [
            .init(
                id: 2,
                isInput: true,
                type: 0,
                name: "Input",
                signalBlocks: [.init(formatCode: 0x06, channelCount: 2)],
                supportedFormats: [.init(sampleRateCode: 0x04, formatCode: 0x06, channelCount: 2)]
            )
        ])
        #expect(backend.fcpCommands == 0)
    }

    @Test func fcpCommandUsesUnitInstanceBoundRouteAndReturnsResponseReceipt() async {
        let backend = FakeLiveDriverBackend()
        backend.devices = [
            device(
                id: 1,
                observedGuid: 0x0011_2233_4455_6677,
                vendorId: 0x0003DB,
                modelId: 0x000001,
                vendorName: "Apogee",
                modelName: "Duet",
                nodeId: 0,
                generation: 17,
                state: .ready,
                units: [],
                deviceKind: 0
            )
        ]
        backend.avcUnits = [
            avcUnit(deviceID: DeviceInstanceID(1),
                    observedGuid: 0x0011_2233_4455_6677,
                    nodeID: 0,
                    vendorID: 0x0003DB,
                    modelID: 1,
                    subunits: [],
                    isoInputPlugs: 1,
                    isoOutputPlugs: 1,
                    extInputPlugs: 1,
                    extOutputPlugs: 1)
        ]
        backend.fcpResponse = Data([0x0C, 0xFF, 0x19])
        let request = ASFWMCPFcpCommandRequest(
            targetUnitID: UnitInstanceID(device: DeviceInstanceID(1), unitDirectoryOffset: 0x28),
            address: ASFWMCPAddress(deviceInstanceId: DeviceInstanceID(1), nodeId: 0,
                                    generation: 17, addressHigh: 0xFFFF,
                                    addressLow: 0xF0000B00),
            intent: .control,
            payload: [0x00, 0xFF, 0x19]
        )

        let receipt = await LiveASFWDriverControl(backend: backend).executeFCPCommand(request)

        #expect(receipt.ok)
        #expect(receipt.observedNodeId == 0)
        #expect(receipt.observedGeneration == 17)
        #expect(receipt.response == [0x0C, 0xFF, 0x19])
        #expect(backend.fcpCommands == 1)
    }

    @Test func busResetWaitsForGenerationChange() async {
        let backend = FakeLiveDriverBackend()
        backend.resetGenerationOnRequest = true
        let control = LiveASFWDriverControl(
            backend: backend,
            transactionTimeout: 0.1,
            pollIntervalNs: 1_000,
            busResetTimeout: 0.1
        )

        let receipt = await control.executeBusReset(
            ASFWMCPBusResetRequest(generation: 17, shortReset: true)
        )

        #expect(receipt.ok)
        #expect(receipt.acceptedGeneration == 17)
        #expect(receipt.observedGeneration == 18)
        #expect(receipt.shortReset)
        #expect(backend.busResetRequests == 1)
    }

    @Test func phase88LifecycleReachesTheDedicatedDriverControl() async {
        let backend = FakeLiveDriverBackend()
        let control = LiveASFWDriverControl(backend: backend)

        let start = await control.executePhase88Streaming(
            endpointID: AudioEndpointID(401),
            start: true
        )
        let stop = await control.executePhase88Streaming(
            endpointID: AudioEndpointID(401),
            start: false
        )

        #expect(start.ok)
        #expect(stop.ok)
        #expect(backend.audioStreamingRequests.count == 2)
        #expect(backend.audioStreamingRequests[0].0 == AudioEndpointID(401))
        #expect(backend.audioStreamingRequests[0].1)
        #expect(backend.audioStreamingRequests[1].0 == AudioEndpointID(401))
        #expect(backend.audioStreamingRequests[1].1 == false)
    }

    @Test func localIrmSnapshotUsesDiagnosticStateInsteadOfAsyncSelfRead() async {
        let backend = FakeLiveDriverBackend()
        backend.localIrmResourceSnapshot = ASFWMCPLocalIrmResourceSnapshot(
            generation: 17,
            localNodeId: 2,
            irmNodeId: 2,
            isLocalIRM: true,
            readbackValid: true,
            bandwidthAvailable: 0x1333,
            channelsAvailable31_0: 0xFFFF_FFFE,
            channelsAvailable63_32: 0xFFFF_FFFF
        )

        let result = await LiveASFWDriverControl(backend: backend).executeIRMSnapshot(
            ASFWMCPIrmSnapshotRequest(generation: 17)
        )

        #expect(result.ok)
        #expect(result.irmNodeId == 2)
        #expect(result.bandwidthAvailable == 0x1333)
        #expect(result.channelsAvailable31_0 == 0xFFFF_FFFE)
        #expect(result.channelsAvailable63_32 == 0xFFFF_FFFF)
        #expect(backend.reads == 0)
        #expect(backend.resultPolls == 0)
    }

    @Test func localIrmSnapshotRejectsUnvalidatedDiagnosticState() async {
        let backend = FakeLiveDriverBackend()
        backend.localIrmResourceSnapshot = ASFWMCPLocalIrmResourceSnapshot(
            generation: 17,
            localNodeId: 2,
            irmNodeId: 2,
            isLocalIRM: true,
            readbackValid: false,
            bandwidthAvailable: 0,
            channelsAvailable31_0: 0,
            channelsAvailable63_32: 0
        )

        let result = await LiveASFWDriverControl(backend: backend).executeIRMSnapshot(
            ASFWMCPIrmSnapshotRequest(generation: 17)
        )

        #expect(!result.ok)
        #expect(result.status == .unavailable)
        #expect(backend.reads == 0)
        #expect(backend.resultPolls == 0)
    }

    private func address(generation: UInt32) -> ASFWMCPAddress {
        ASFWMCPAddress(
            deviceInstanceId: DeviceInstanceID(1),
            nodeId: 0xFFC1,
            generation: generation,
            addressHigh: 0xFFFF,
            addressLow: 0xF0000400
        )
    }

    private func configRomFixture() -> Data {
        let quadlets: [UInt32] = [
            0x0404_0000, // q0: BIB has q1...q4
            0x3133_3934, // q1: "1394"
            0xE0FF_8112, // q2: IRMC/CMC/ISC, max_rec=8, max_rom=1, gen=1, S400
            0x0013_0E04, // q3: GUID high
            0x0200_4713, // q4: GUID low
            0x0002_0000, // q5: root directory with two entries
            0x0300_130E, // q6: VENDOR immediate
            0x1700_0008  // q7: MODEL immediate
        ]
        return Data(quadlets.flatMap { word in
            [
                UInt8((word >> 24) & 0xFF), UInt8((word >> 16) & 0xFF),
                UInt8((word >> 8) & 0xFF), UInt8(word & 0xFF)
            ]
        })
    }

    private func device(
        id: UInt64,
        observedGuid: UInt64,
        vendorId: UInt32,
        modelId: UInt32,
        vendorName: String,
        modelName: String,
        nodeId: UInt16,
        generation: UInt32,
        state: DriverConnectorFWDeviceState,
        units: [FWUnitInfo],
        deviceKind: UInt8
    ) -> FWDeviceInfo {
        FWDeviceInfo(
            id: DeviceInstanceID(id),
            observedGuid: observedGuid,
            nodeVendorOui: vendorId,
            rootVendorId: vendorId,
            rootModelId: modelId,
            vendorName: vendorName,
            modelName: modelName,
            nodeId: nodeId,
            generation: generation,
            state: state,
            quarantineReason: .none,
            rawBusInfoQuadlets: [],
            units: units,
            deviceKind: deviceKind
        )
    }

    private func avcUnit(
        deviceID: DeviceInstanceID,
        observedGuid: UInt64,
        nodeID: UInt16,
        vendorID: UInt32,
        modelID: UInt32,
        subunits: [AVCSubunitInfo],
        isoInputPlugs: UInt8,
        isoOutputPlugs: UInt8,
        extInputPlugs: UInt8,
        extOutputPlugs: UInt8
    ) -> AVCUnitInfo {
        AVCUnitInfo(
            id: UnitInstanceID(device: deviceID, unitDirectoryOffset: 0x28),
            observedGuid: observedGuid,
            generation: 17,
            nodeID: nodeID,
            isInitialized: true,
            rootVendorID: vendorID,
            rootModelID: modelID,
            unitVendorID: vendorID,
            unitModelID: modelID,
            specifierID: 0x00A02D,
            unitVersion: 0x000101,
            subunits: subunits,
            isoInputPlugs: isoInputPlugs,
            isoOutputPlugs: isoOutputPlugs,
            extInputPlugs: extInputPlugs,
            extOutputPlugs: extOutputPlugs
        )
    }

    private func sbp2Unit() -> FWUnitInfo {
        FWUnitInfo(
            id: UnitInstanceID(device: DeviceInstanceID(2), unitDirectoryOffset: 0x30),
            specId: 0x00609E,
            swVersion: 0x010483,
            vendorId: nil,
            modelId: nil,
            state: .ready,
            romOffset: 0,
            managementAgentOffset: 0x100,
            lun: 0,
            unitCharacteristics: nil,
            fastStart: nil,
            vendorName: "Mock Storage",
            productName: "Disk"
        )
    }

    private func capabilitiesFixture() -> AVCMusicCapabilities? {
        var data = Data(repeating: 0, count: 18)
        data[0] = 0x03
        data[1] = 0x04
        data.replaceSubrange(2..<6, with: [0x18, 0x00, 0x00, 0x00])
        data[8] = 1
        data[9] = 1
        data[10] = 1
        data[14] = 1

        var plug = Data(repeating: 0, count: 40)
        plug[0] = 2
        plug[1] = 1
        plug[2] = 0
        plug[3] = 1
        plug[4] = 5
        plug.replaceSubrange(5..<10, with: Array("Input".utf8))
        plug[37] = 1
        data.append(plug)
        data.append(contentsOf: [0x06, 0x02, 0x00, 0x00])
        data.append(contentsOf: [0x04, 0x06, 0x02, 0x00])
        return AVCMusicCapabilities(data: data)
    }
}

@MainActor
private final class FakeLiveDriverBackend: ASFWLiveDriverBackend {
    var mcpIsConnected = true
    var mcpLastError: String?
    var generation: UInt32 = 17
    var devices: [FWDeviceInfo] = []
    var topologySnapshot: TopologySnapshot?
    var configROM: ASFWDriverConnector.ConfigROMFetchResult?
    var avcUnits: [AVCUnitInfo] = []
    var avcSubunitCapabilities: AVCMusicCapabilities?
    var nextHandle: UInt16 = 0x44
    var results: [UInt16: ASFWDriverConnector.AsyncTransactionResult] = [:]
    var reads = 0
    var blockReads = 0
    var writes = 0
    var blockWrites = 0
    var compareSwaps = 0
    var resultPolls = 0
    var fcpCommands = 0
    var fcpResponse: Data?
    var signalFormatProbes: [(UnitInstanceID, SignalFormatPlugDirection, UInt8)] = []
    var signalFormatProbeResult: SignalFormatProbeResult?
    var audioStreamingRequests: [(AudioEndpointID, Bool)] = []
    var busResetRequests = 0
    var resetGenerationOnRequest = false
    var localIrmResourceSnapshot: ASFWMCPLocalIrmResourceSnapshot?
    var logQueryResponse: ASFWLogRingQueryResponse?
    var logStats: ASFWLogRingStats?

    func mcpCurrentGeneration() -> UInt32? { generation }
    func mcpControllerStatus() -> ControllerStatus? { nil }
    func mcpFetchDiagnostics() throws -> ASFWDiagnosticsSnapshot { throw DiagnosticsError.notConnected }
    func mcpLocalIrmResourceSnapshot() -> ASFWMCPLocalIrmResourceSnapshot? {
        localIrmResourceSnapshot
    }
    func mcpDiscoveredDevices() -> [FWDeviceInfo]? { devices }
    func mcpTopologySnapshot() -> TopologySnapshot? { topologySnapshot }
    func mcpConfigROM(deviceID: DeviceInstanceID,
                      expectedGeneration: UInt16) -> ASFWDriverConnector.ConfigROMFetchResult? {
        configROM
    }
    func mcpAVCUnits() -> [AVCUnitInfo]? { avcUnits }
    func mcpAVCSubunitCapabilities(unitID: UnitInstanceID, type: UInt8, id: UInt8) -> AVCMusicCapabilities? {
        avcSubunitCapabilities
    }

    func mcpAsyncRead(deviceID: DeviceInstanceID, addressHigh: UInt16, addressLow: UInt32, length: UInt32) -> UInt16? {
        reads += 1
        return nextHandle
    }

    func mcpAsyncWrite(deviceID: DeviceInstanceID, addressHigh: UInt16, addressLow: UInt32, payload: Data) -> UInt16? {
        writes += 1
        return nextHandle
    }

    func mcpAsyncBlockRead(deviceID: DeviceInstanceID, addressHigh: UInt16, addressLow: UInt32, length: UInt32) -> UInt16? {
        blockReads += 1
        return nextHandle
    }

    func mcpAsyncBlockWrite(deviceID: DeviceInstanceID, addressHigh: UInt16, addressLow: UInt32, payload: Data) -> UInt16? {
        blockWrites += 1
        return nextHandle
    }

    func mcpAsyncCompareSwap(deviceID: DeviceInstanceID, addressHigh: UInt16, addressLow: UInt32, compareValue: Data, newValue: Data) -> UInt16? {
        compareSwaps += 1
        return nextHandle
    }

    func mcpTransactionResult(handle: UInt16, initialPayloadCapacity: Int) -> ASFWDriverConnector.AsyncTransactionResult? {
        resultPolls += 1
        return results[handle]
    }

    func mcpSendRawFCPCommand(unitID: UnitInstanceID, frame: Data, timeoutMs: UInt32) -> Data? {
        fcpCommands += 1
        return fcpResponse
    }

    func mcpProbeSignalFormat(unitID: UnitInstanceID,
                              plugDirection: SignalFormatPlugDirection,
                              plugID: UInt8,
                              timeoutMs: UInt32) -> SignalFormatProbeResult? {
        signalFormatProbes.append((unitID, plugDirection, plugID))
        return signalFormatProbeResult
    }

    func mcpSetAudioStreaming(endpointID: AudioEndpointID, enabled: Bool) -> Int32 {
        audioStreamingRequests.append((endpointID, enabled))
        return 0
    }

    func mcpRequestUserBusReset(expectedGeneration: UInt32, shortReset: Bool) -> UInt32? {
        guard expectedGeneration == generation else { return nil }
        busResetRequests += 1
        if resetGenerationOnRequest {
            generation &+= 1
        }
        return expectedGeneration
    }

    func mcpQueryLogRecords(_ query: ASFWLogRingQuery) -> ASFWLogRingQueryResponse? {
        logQueryResponse
    }

    func mcpLogRingStats() -> ASFWLogRingStats? {
        logStats
    }

    // Audio telemetry is not exercised by these transaction-path tests; the
    // stream-health projection is covered where the counters are produced.
    func mcpAudioTelemetry() -> AudioTelemetrySnapshot? {
        nil
    }
}
