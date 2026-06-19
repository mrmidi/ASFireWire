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
            FWDeviceInfo(
                id: 0x0011_2233_4455_6677,
                guid: 0x0011_2233_4455_6677,
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
            FWDeviceInfo(
                id: 0x00AA_BBCC_DDEE_FF00,
                guid: 0x00AA_BBCC_DDEE_FF00,
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
            AVCUnitInfo(
                guid: 0x0011_2233_4455_6677,
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

    private func address(generation: UInt32) -> ASFWMCPAddress {
        ASFWMCPAddress(
            nodeId: 0xFFC1,
            generation: generation,
            addressHigh: 0xFFFF,
            addressLow: 0xF0000400
        )
    }

    private func sbp2Unit() -> FWUnitInfo {
        FWUnitInfo(
            specId: 0x00609E,
            swVersion: 0x010483,
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
}

@MainActor
private final class FakeLiveDriverBackend: ASFWLiveDriverBackend {
    var mcpIsConnected = true
    var mcpLastError: String?
    var generation: UInt32 = 17
    var devices: [FWDeviceInfo] = []
    var avcUnits: [AVCUnitInfo] = []
    var nextHandle: UInt16 = 0x44
    var results: [UInt16: ASFWDriverConnector.AsyncTransactionResult] = [:]
    var reads = 0
    var blockReads = 0
    var writes = 0
    var blockWrites = 0
    var compareSwaps = 0
    var resultPolls = 0

    func mcpCurrentGeneration() -> UInt32? { generation }
    func mcpControllerStatus() -> ControllerStatus? { nil }
    func mcpFetchDiagnostics() throws -> ASFWDiagnosticsSnapshot { throw DiagnosticsError.notConnected }
    func mcpDiscoveredDevices() -> [FWDeviceInfo]? { devices }
    func mcpAVCUnits() -> [AVCUnitInfo]? { avcUnits }

    func mcpAsyncRead(destinationID: UInt16, addressHigh: UInt16, addressLow: UInt32, length: UInt32) -> UInt16? {
        reads += 1
        return nextHandle
    }

    func mcpAsyncWrite(destinationID: UInt16, addressHigh: UInt16, addressLow: UInt32, payload: Data) -> UInt16? {
        writes += 1
        return nextHandle
    }

    func mcpAsyncBlockRead(destinationID: UInt16, addressHigh: UInt16, addressLow: UInt32, length: UInt32) -> UInt16? {
        blockReads += 1
        return nextHandle
    }

    func mcpAsyncBlockWrite(destinationID: UInt16, addressHigh: UInt16, addressLow: UInt32, payload: Data) -> UInt16? {
        blockWrites += 1
        return nextHandle
    }

    func mcpAsyncCompareSwap(destinationID: UInt16, addressHigh: UInt16, addressLow: UInt32, compareValue: Data, newValue: Data) -> UInt16? {
        compareSwaps += 1
        return nextHandle
    }

    func mcpTransactionResult(handle: UInt16, initialPayloadCapacity: Int) -> ASFWDriverConnector.AsyncTransactionResult? {
        resultPolls += 1
        return results[handle]
    }
}
