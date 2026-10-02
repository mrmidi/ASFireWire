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
        #expect(text.contains("<unavailable: reason not recorded in this export>"))
        #expect(text.contains("0 bytes (empty)"))
        snapshot.devices[0].avcUnit = nil
        #expect(AvcReportTextFormatter.format(snapshot).contains("AV/C UNIT: <unavailable"))
    }

    @Test func missingBlobShowsTheDriversReasonNotTheExportLimit() {
        var snapshot = report()
        snapshot.devices[0].avcUnit?.subunits[0].descriptor = nil
        snapshot.devices[0].avcUnit?.subunits[0].descriptorMissing = "the driver has not read this descriptor from the device"
        let text = AvcReportTextFormatter.format(snapshot)
        #expect(text.contains("<unavailable: the driver has not read this descriptor from the device>"))
        #expect(!text.contains("export limit"))
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

    // MARK: - FCP exchange log

    private static let unitInfoExchange = AvcReportSnapshot.Exchange(
        sequence: 7, generation: 4, outcome: "response", interim: true, retries: 1,
        command: [0x01, 0xFF, 0x30, 0xFF], response: [0x0C, 0xFF, 0x30])

    @Test func parsesTheDriversExchangePage() throws {
        // Layout from UserClient/WireFormats/AVCExchangeLogWire.hpp, little-endian.
        var page: [UInt8] = []
        func u32(_ v: UInt32) { page += [UInt8(v & 0xFF), UInt8(v >> 8 & 0xFF), UInt8(v >> 16 & 0xFF), UInt8(v >> 24)] }
        func u16(_ v: UInt16) { page += [UInt8(v & 0xFF), UInt8(v >> 8)] }
        u32(3); u32(2); u32(2); u32(0); u32(2); u32(0)
        u32(7); u32(4); page += [0, 1, 1, 0]; u16(4); u16(3)
        page += [0x01, 0xFF, 0x30, 0xFF, 0x0C, 0xFF, 0x30, 0x00]
        u32(8); u32(4); page += [1, 0, 0, 0]; u16(3); u16(0)
        page += [0x01, 0xFF, 0x31, 0x00]
        let parsed = try #require(AvcReportSnapshot.ExchangeLog.parsePage(Data(page)))
        #expect(parsed.session == 3)
        #expect(parsed.dropped == 2)
        #expect(parsed.totalRecords == 2)
        #expect(parsed.records.first == Self.unitInfoExchange)
        #expect(parsed.records.last?.outcome == "timeout")
        #expect(parsed.records.last?.response.isEmpty == true)
        #expect(AvcReportSnapshot.ExchangeLog.parsePage(Data(page.prefix(30))) == nil)
    }

    @Test func refreshCarriesTheExchangeLogIntoTheReport() async throws {
        let source = Source()
        source.status = 0x82
        source.exchangeLog = .init(session: 2, dropped: 0, records: [Self.unitInfoExchange])
        let store = AvcReportStore(connector: source)
        await store.refresh()
        let device = try #require(store.snapshot?.devices.first)
        #expect(device.exchanges?.records == [Self.unitInfoExchange])
        #expect(store.reportText.contains("1 exchanges in session 2: STABLE 1"))
        #expect(store.reportText.contains("#0007 g4 STABLE after INTERIM after 1 retries"))
        #expect(store.reportText.contains("> 01 FF 30 FF"))
        #expect(store.reportText.contains("< 0C FF 30"))
    }

    @Test func listEndIsNotCountedAsAnError() async throws {
        let source = Source()
        source.status = 0x82
        source.exchangeLog = .init(session: 1, dropped: 0, records: [
            .init(sequence: 1, generation: 1, outcome: "response", interim: false, retries: 0,
                  command: [0x01, 0xFF, 0x2F, 0xC1, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0x05, 0x00],
                  response: [0x0A, 0xFF, 0x2F, 0xC1, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0x05, 0x00]),
            .init(sequence: 2, generation: 1, outcome: "response", interim: false, retries: 0,
                  command: [0x01, 0xFF, 0x30, 0xFF], response: [0x0A, 0xFF, 0x30, 0xFF])])
        let store = AvcReportStore(connector: source)
        await store.refresh()
        #expect(store.reportText.contains("end of list 1"))
        #expect(store.reportText.contains("REJECTED 1"))
        #expect(store.reportText.contains("#0001 g1 end of list (REJECTED)"))
    }

    @Test func unitPlugsAreNotReportedWhenNoDiscoveryRan() async throws {
        let source = Source()
        source.status = 0x84  // skipped: the probe policy sends nothing
        let store = AvcReportStore(connector: source)
        await store.refresh()
        let device = try #require(store.snapshot?.devices.first)
        #expect(device.avcUnit == nil)
        #expect(device.notes.contains { $0.contains("were not read") })
        #expect(!store.reportText.contains("ISO inputs (playback): 0"))
    }

    @Test func versionOneDumpsStillOpen() throws {
        var snapshot = report()
        snapshot.schemaVersion = 1
        let restored = try AvcReportSnapshot.load(snapshot.jsonData())
        #expect(restored.devices.first?.exchanges == nil)
    }

    @Test func binaryExportWritesAReplayableExchangeDump() throws {
        var snapshot = report()
        snapshot.devices[0].exchanges = .init(session: 1, dropped: 0, records: [Self.unitInfoExchange])
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        defer { try? FileManager.default.removeItem(at: directory) }
        try AvcReportExporter.export(snapshot, to: directory)
        let file = directory.appendingPathComponent("0003DB0000000001-fcp-exchanges.json")
        let json = try #require(JSONSerialization.jsonObject(with: Data(contentsOf: file)) as? [String: Any])
        let device = try #require(json["device"] as? [String: Any])
        #expect(device["guid"] as? String == "0x0003db0000000001")
        let records = try #require(json["records"] as? [[String: Any]])
        #expect(records.first?["command"] as? [Int] == [0x01, 0xFF, 0x30, 0xFF])
        #expect(records.first?["response"] as? [Int] == [0x0C, 0xFF, 0x30])
    }

    // MARK: Discovery document (phase 4.5)

    private static let sampleDocument = Data(#"""
    {"format":"asfw.avc.discovery","version":1,"session":3,"route":{"guid":"0x000aac0000000003","generation":4,"node":1},
     "snapshot":{"complete":true,"cancelled":false,"terminalError":null,"probeCount":12,
       "failedProbes":[{"address":255,"opcode":48,"error":{"kind":"timeout","response":null,"operandOffset":0}}]},
     "graph":{"playback":{"channels":10,"dataBlockSize":11,"midi":1,"rate":48000,"rates":[44100,48000],"channelNames":["Out 1","Out 2"]},
              "capture":{"channels":10,"dataBlockSize":11,"midi":1,"rate":48000,"rates":[48000],"channelNames":[]}},
     "exchanges":{"session":3,"dropped":0,"records":[{"elapsedUs":1500},{"elapsedUs":500}]}}
    """#.utf8)

    /// Pages laid out as the driver serves them (AVCDiscoveryPageWire).
    private static func pages(_ document: Data, session: UInt32 = 3, chunk: Int = 64) -> [UInt32: Data] {
        func le(_ value: UInt32) -> [UInt8] { (0..<4).map { UInt8(truncatingIfNeeded: value >> (8 * $0)) } }
        func le16(_ value: UInt16) -> [UInt8] { [UInt8(value & 0xFF), UInt8(value >> 8)] }
        var result: [UInt32: Data] = [:]
        var offset = 0
        repeat {
            let length = min(chunk, document.count - offset)
            var page = le(AvcDiscoveryDocument.PageHeader.magic) + le16(1) + le16(32) + le(session) + le(4)
            page += le(UInt32(document.count)) + le(UInt32(offset)) + le(UInt32(length)) + le(AvcDiscoveryDocument.fnv1a(document))
            result[UInt32(offset)] = Data(page) + document.subdata(in: offset..<(offset + length))
            offset += length
        } while offset < document.count
        return result
    }

    @Test func discoveryPagesAssembleIntoTheDocument() throws {
        let pages = Self.pages(Self.sampleDocument)
        #expect(pages.count > 1)
        let assembled = try AvcDiscoveryDocument.assemble { pages[$0] }.get()
        #expect(assembled == Self.sampleDocument)
    }

    @Test func pagesFromTwoDocumentsAreRejected() {
        let first = Self.pages(Self.sampleDocument, session: 3)
        let second = Self.pages(Self.sampleDocument, session: 4)
        // A refresh landed between the first and second page.
        let result = AvcDiscoveryDocument.assemble { $0 == 0 ? first[0] : second[$0] }
        #expect(result == .failure(.mixedPages))
    }

    @Test func aCorruptedDocumentFailsItsChecksum() {
        var pages = Self.pages(Self.sampleDocument)
        var last = pages[pages.keys.max()!]!
        last[last.count - 2] ^= 0x01
        pages[pages.keys.max()!] = last
        #expect(AvcDiscoveryDocument.assemble { pages[$0] } == .failure(.checksumMismatch))
    }

    @Test func refreshCarriesTheDiscoveryDocumentIntoTheReport() async throws {
        let source = Source()
        source.status = 0x82
        source.discoveryDocument = Self.sampleDocument
        let store = AvcReportStore(connector: source)
        await store.refresh()
        let device = try #require(store.snapshot?.devices.first)
        #expect(device.discovery != nil)
        #expect(store.reportText.contains("DISCOVERY (driver document v1, session 3)"))
        #expect(store.reportText.contains("Result: complete"))
        #expect(store.reportText.contains("Probes: 12, failed: 1"))
        #expect(store.reportText.contains("Playback: 10 PCM + 1 MIDI, block 11, 48000 Hz (rates: 44100, 48000)"))
        #expect(store.reportText.contains("total time 2.0 ms"))
        // The export keeps the document whole and still opens.
        let restored = try AvcReportSnapshot.load(try #require(store.snapshot).jsonData())
        #expect(restored.schemaVersion == 3)
        #expect(restored.devices.first?.discovery == device.discovery)
    }

    @Test func aCancelledDiscoveryNeverReplacesTheReport() async {
        let source = Source()
        source.status = 0x82
        let store = AvcReportStore(connector: source)
        await store.refresh()
        let text = store.reportText
        source.discoveryDocument = Data(String(decoding: Self.sampleDocument, as: UTF8.self)
            .replacingOccurrences(of: #""cancelled":false"#, with: #""cancelled":true"#).utf8)
        await store.refresh()
        #expect(store.reportText == text)
        #expect(store.error?.contains("cancelled") == true)
    }

    @Test func aStreamingDeviceIsReportedNotProbed() async throws {
        let source = Source()
        source.status = 0x85
        let store = AvcReportStore(connector: source)
        await store.refresh()
        let device = try #require(store.snapshot?.devices.first)
        #expect(device.notes.contains { $0.contains("audio is active") })
        #expect(device.avcUnit == nil)
    }

    @Test func versionTwoDumpsStillOpen() throws {
        var snapshot = report()
        snapshot.schemaVersion = 2
        let restored = try AvcReportSnapshot.load(snapshot.jsonData())
        #expect(restored.devices.first?.discovery == nil)
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
        func subunitCapabilitiesBlob(guid: UInt64, type: UInt8, id: UInt8) -> Result<Data, AvcBlobUnavailable> {
            .failure(.init(reason: "stub"))
        }
        func subunitDescriptorBlob(guid: UInt64, type: UInt8, id: UInt8) -> Result<Data, AvcBlobUnavailable> {
            .failure(.init(reason: "the driver has not read this descriptor from the device"))
        }
        var exchangeLog: AvcReportSnapshot.ExchangeLog?
        func getFCPExchangeLog(guid: UInt64) -> AvcReportSnapshot.ExchangeLog? { exchangeLog }
        var discoveryDocument: Data?
        func getAVCDiscoveryDocument(guid: UInt64) -> Data? { discoveryDocument }
    }
}
