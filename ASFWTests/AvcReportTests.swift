import Foundation
import Testing
@testable import ASFW

@MainActor
struct AvcReportTests {
    private func report() -> AvcReportSnapshot {
        .init(capturedAt: Date(timeIntervalSince1970: 1_700_000_000), appVersion: "0.3.1", driverVersion: "abcdef12",
            devices: [.init(guid: 0x0003DB0000000001, nodeID: 1, generation: 7,
                vendorID: 0x0003DB, modelID: 0x01DDDD, vendorName: "Apogee", modelName: "Duet", state: "ready",
                romUnits: [.init(offset: 12, specifierID: 0x00A02D, version: 0x010001)],
                configROM: Data([0x31, 0x33, 0x39, 0x34]),
                avcUnit: .init(isoInputPlugs: 1, isoOutputPlugs: 1, externalInputPlugs: 0, externalOutputPlugs: 0,
                    subunits: [.init(type: 0x0C, id: 0, sourcePlugs: 2, destinationPlugs: 1,
                        capabilitySummary: "  Plug 1 source (capture): Mic 1", capabilities: Data([1, 2, 3]),
                        descriptor: Data([0x81, 0x00, 0x00, 0x04]))]), notes: [])])
    }

    @Test func dumpRoundTripPreservesEvidence() throws {
        let original = report()
        let restored = try AvcReportSnapshot.load(original.jsonData())
        let device = try #require(restored.devices.first)
        #expect(device.guid == original.devices.first?.guid)
        #expect(device.configROM == Data([0x31, 0x33, 0x39, 0x34]))
        #expect(device.avcUnit?.subunits.first?.descriptor == Data([0x81, 0x00, 0x00, 0x04]))
        #expect(device.avcUnit?.subunits.first?.capabilities == Data([1, 2, 3]))
        #expect(AvcReportTextFormatter.format(restored) == AvcReportTextFormatter.format(original))
    }

    @Test func reportExposesIdentityAndRawBytes() {
        let text = AvcReportTextFormatter.format(report())
        #expect(text.contains("0x0003DB0000000001"))
        #expect(text.contains("generation: 7"))
        #expect(text.contains("Apogee Duet"))
        #expect(text.contains("0x00A02D"))
        #expect(text.contains("0000  81 00 00 04"))
        #expect(text.contains("Mic 1"))
        #expect(text.contains("not an AV/C reply"))
        #expect(text.contains("ASFW"))
    }

    @Test func decodedRateSummaryUsesAvcRateCodes() throws {
        var wire = Data(repeating: 0, count: 18)
        wire[0] = 1
        wire[1] = 0x0A // AV/C 88.2 kHz, not the CIP SFC table.
        wire[5] = 4 // Supported mask bit 10.
        let caps = try #require(AVCMusicCapabilities(data: wire))
        let text = AvcReportTextFormatter.capabilitiesSummary(caps)
        #expect(text.contains("Current rate: 88.2 kHz"))
        #expect(text.contains("Supported rates: 88.2 kHz"))
        #expect(text.contains("0x00000400"))
    }

    @Test func unavailableAndEmptyRemainDifferent() {
        var snapshot = report()
        snapshot.devices[0].avcUnit?.subunits[0].capabilities = nil
        snapshot.devices[0].avcUnit?.subunits[0].descriptor = Data()
        let text = AvcReportTextFormatter.format(snapshot)
        #expect(text.contains("<unavailable; export limit"))
        #expect(text.contains("0 bytes (empty)"))
        snapshot.devices[0].avcUnit = nil
        #expect(AvcReportTextFormatter.format(snapshot).contains("AV/C UNIT: <unavailable"))
    }

    @Test func rejectsFutureSchemaWithoutReplacingData() throws {
        var snapshot = report()
        snapshot.schemaVersion = 99
        let data = try snapshot.jsonData()
        #expect(throws: AvcReportSnapshot.ImportError.self) { try AvcReportSnapshot.load(data) }
    }

    @Test func rejectsOversizedBlob() throws {
        var snapshot = report()
        snapshot.devices[0].configROM = Data(repeating: 0, count: 4097)
        let data = try snapshot.jsonData()
        #expect(throws: AvcReportSnapshot.ImportError.self) { try AvcReportSnapshot.load(data) }
    }

    @Test func opensOfflineAndRetainsReportOnInvalidImport() throws {
        let url = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString + ".json")
        defer { try? FileManager.default.removeItem(at: url) }
        try report().jsonData().write(to: url)
        let store = AvcReportStore(connector: ASFWDriverConnector())
        store.openDump(url)
        #expect(store.isImported)
        #expect(store.deviceCount == 1)
        #expect(store.error == nil)
        let text = store.reportText
        try Data("invalid JSON".utf8).write(to: url)
        store.openDump(url)
        #expect(store.error != nil)
        #expect(store.reportText == text)
    }

    @Test func capturesUncataloguedAvcWithoutAnInventory() async throws {
        let source = Source()
        let store = AvcReportStore(connector: source)
        await store.refresh()
        let device = try #require(store.snapshot?.devices.first)
        #expect(device.vendorID == 0xABCDEF)
        #expect(device.avcUnit == nil)
        #expect(device.configROM == Data([1, 2, 3, 4]))
        #expect(store.deviceCount == 1)
        #expect(store.error == nil)
        #expect(store.reportText.contains("no unit was available"))
    }

    @Test func routeChangeRetainsPreviousSnapshot() async {
        let source = Source()
        let store = AvcReportStore(connector: source)
        await store.refresh()
        let text = store.reportText
        source.changeRoute = true
        await store.refresh()
        #expect(store.reportText == text)
        #expect(store.error?.contains("routing changed") == true)
        #expect(store.isRefreshing == false)
    }

    @Test func refreshRequestsDiscoveryAndRecordsFailures() async throws {
        let source = Source()
        source.status = 0x83
        let store = AvcReportStore(connector: source)
        await store.refresh()
        #expect(source.rescans == 1)
        let device = try #require(store.snapshot?.devices.first)
        #expect(device.notes.contains { $0.contains("discovery failed") })
    }

    @Test func refreshWaitsForDiscoveryCompletion() async throws {
        let source = Source()
        source.status = 0x81
        source.completeAfterPolls = true
        let store = AvcReportStore(connector: source)
        await store.refresh()
        #expect(source.rescans == 1)
        #expect(source.polls >= 3)
        #expect(store.snapshot?.devices.first?.notes.contains { $0 == "Manual discovery completed." } == true)
    }

    @Test func olderDriverCannotClaimLiveCapture() async {
        let source = Source()
        source.status = 0
        let store = AvcReportStore(connector: source)
        await store.refresh()
        #expect(store.snapshot == nil)
        #expect(source.rescans == 0)
        #expect(store.error?.contains("completion") == true)
    }

    @Test func binaryExportPreservesOriginalBytesAndParsedOrder() throws {
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: directory) }
        let manifestURL = try AvcReportExporter.export(report(), to: directory)
        #expect(FileManager.default.fileExists(atPath: manifestURL.path))
        #expect(try Data(contentsOf: directory.appendingPathComponent("0003DB0000000001-config-rom.bin")) == Data([0x31, 0x33, 0x39, 0x34]))
        #expect(try Data(contentsOf: directory.appendingPathComponent("0003DB0000000001-subunit-0C-00-descriptor.bin")) == Data([0x81, 0, 0, 4]))
        let restored = try AvcReportSnapshot.load(Data(contentsOf: directory.appendingPathComponent("snapshot.json")))
        #expect(restored.devices.first?.guid == report().devices.first?.guid)
        let text = AvcReportTextFormatter.format(report())
        let parsed = try #require(text.range(of: "Mic 1"))
        let raw = try #require(text.range(of: "RAW BINARY APPENDIX"))
        #expect(parsed.lowerBound < raw.lowerBound)
        #expect(throws: CocoaError.self) { try AvcReportExporter.export(report(), to: directory) }
    }

    @MainActor
    private final class Source: AvcReportSource {
        var isConnected = true
        var changeRoute = false
        var status: UInt8?
        var rescans = 0
        var polls = 0
        var completeAfterPolls = false
        private var reads = 0
        func getDiscoveredDevices() -> [FWDeviceInfo]? {
            reads += 1
            return [.init(id: 123, guid: 123, vendorId: 0xABCDEF, modelId: 987,
                vendorName: "Uncatalogued", modelName: "AV/C", nodeId: 1,
                generation: changeRoute && reads.isMultiple(of: 2) ? 8 : 7,
                state: .created, units: [.init(specId: 0x00A02D, swVersion: 0x010001, state: .created,
                    romOffset: 12, managementAgentOffset: nil, lun: nil, unitCharacteristics: nil,
                    fastStart: nil, vendorName: nil, productName: nil)], deviceKind: 0)]
        }
        func reScanAVCUnits() -> Bool { rescans += 1; return true }
        func getAVCUnits() -> [AVCUnitInfo]? {
            polls += 1
            if completeAfterPolls && polls >= 3 { status = 0x82 }
            guard let status else { return [] }
            return [.init(guid: 123, nodeID: 1, vendorID: 0xABCDEF, modelID: 987,
                          subunits: [], isoInputPlugs: 0, isoOutputPlugs: 0,
                          extInputPlugs: 0, extOutputPlugs: 0, diagnosticStatus: status)]
        }
        func getDriverVersion() -> DriverVersionInfo? { nil }
        func getConfigROM(nodeId: UInt8, generation: UInt16) -> ASFWDriverConnector.ConfigROMFetchResult? {
            .init(data: Data([1, 2, 3, 4]), requestedGeneration: generation, resolvedGeneration: generation)
        }
        func getSubunitCapabilitiesData(guid: UInt64, type: UInt8, id: UInt8) -> Data? { nil }
        func getSubunitDescriptor(guid: UInt64, type: UInt8, id: UInt8) -> Data? { nil }
    }
}
