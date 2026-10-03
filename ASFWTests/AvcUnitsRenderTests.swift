import AppKit
import SwiftUI
import Testing
@testable import ASFW

/// Decodes the Phase 88 discovery document (the driver's golden for it) and checks what the screen reads from it;
/// renders each tab to a PNG so the layout can be looked at (set ASFW_RENDER_DIR to keep them).
@MainActor
struct AvcUnitsRenderTests {
    static func fixtureData() throws -> Data {
        let url = URL(fileURLWithPath: #filePath).deletingLastPathComponent().deletingLastPathComponent()
            .appendingPathComponent("tests/golden/avc/phase88__discovery_document.json")
        return try Data(contentsOf: url)
    }

    static func dashboard() throws -> AvcUnitDashboard {
        let document = try #require(AvcUnitDocument.decode(try fixtureData()))
        return AvcUnitDashboard(guid: 0x000A_AC03_00B1_D1F7, nodeID: 1, vendorID: 0x000AAC, modelID: 3,
                                vendorName: "TerraTec Electronic GmbH", modelName: "PHASE 88 Rack FW", deviceState: "ready",
                                wireUnit: nil, document: document)
    }

    @Test func documentDecodesWithTheFactsTheScreenShows() throws {
        let unit = try Self.dashboard()
        #expect(unit.health == .complete)
        #expect(unit.sampleRate == 48000)
        #expect(unit.supportedRates == [32000, 44100, 48000, 88200, 96000])
        #expect(unit.playback?.channels == 10 && unit.capture?.channels == 10)
        #expect(unit.midiLabels == ["MidiPort_1", "MidiPort_2"])
        #expect(unit.musicIdentifier?.versionText == "1.0")
        #expect(unit.musicIdentifier?.audioSyncName == "bus+external")
        #expect(unit.musicIdentifier?.general?.receiveName == "non-blocking+blocking")
        #expect(unit.audioBlocks.count == 22)
        #expect(unit.subunits.map(\.typeName) == ["Audio", "Music"])
        // Selectors carry their names and what each input is.
        let clock = try #require(unit.routing.first { $0.title == "Clock Selector" })
        #expect(clock.inputs == ["not connected", "external Clocksource Selector"])
        #expect(clock.current == 0)
        // Controls: grouped by block, master first.
        let controls = unit.controlBlocks
        #expect(controls.first?.title == "Mixer Output Level")
        #expect(controls.first?.channels.first?.isMaster == true)
        #expect(controls.first?.channels.first?.muted == false)
        #expect(controls.first?.channels.first?.volumeDb == 0)
    }

    @Test func channelNamesLoseTheirSharedSuffix() {
        let names = ["Line_1/2 left PHASE88 FW", "Line_1/2 right PHASE88 FW", "SPDIF left PHASE88 FW"]
        #expect(AvcUnitDashboard.trimCommonSuffix(names) == ["Line_1/2 left", "Line_1/2 right", "SPDIF left"])
        #expect(AvcUnitDashboard.trimCommonSuffix(["Mic", "Line"]) == ["Mic", "Line"])
        #expect(AvcUnitDashboard.trimCommonSuffix(["Only"]) == ["Only"])
    }

    @Test func aDocumentFromAnOlderDriverStillDecodes() throws {
        // No contents, no decoded values: the screen shows what it has.
        let json = #"{"format":"asfw.avc.discovery","version":1,"session":3,"snapshot":{"complete":false,"cancelled":false,"terminalError":{"kind":"timeout"}}}"#
        let document = try #require(AvcUnitDocument.decode(Data(json.utf8)))
        let unit = AvcUnitDashboard(guid: 1, nodeID: 0, vendorID: 0, modelID: 0, vendorName: nil, modelName: nil, deviceState: nil, wireUnit: nil, document: document)
        #expect(unit.health == .partial("timeout"))
        #expect(unit.controlBlocks.isEmpty && unit.routing.isEmpty && unit.audioBlocks.isEmpty)
        #expect(AvcUnitDocument.decode(Data(#"{"format":"other","version":1,"session":0}"#.utf8)) == nil)
    }

    @Test func legacyDocumentPreservesProbeIdentityAndSignalsMissingExtension() throws {
        let json = #"{"format":"asfw.avc.discovery","version":1,"session":1,"snapshot":{"complete":true,"cancelled":false,"failedProbes":[{"address":8,"opcode":184,"error":{"kind":"unexpectedResponse"}},{"address":96,"opcode":9,"error":{"kind":"unexpectedResponse"}}],"descriptors":[{"subunit":{"type":1,"id":0},"bytes":56,"data":"0003"}]}}"#
        let document = try #require(AvcUnitDocument.decode(Data(json.utf8)))
        let unit = AvcUnitDashboard(guid: 1, nodeID: 1, vendorID: 0, modelID: 0, vendorName: nil, modelName: nil, deviceState: nil, wireUnit: nil, document: document)
        #expect(unit.needsDocumentUpdate)
        #expect(unit.document?.snapshot?.descriptors?.first?.bytes == 56)
        #expect(unit.failedProbeGroups.count == 2)
        #expect(unit.failedProbeGroups[0].address == "Address 0x08")
        #expect(unit.failedProbeGroups[0].opcode == "Opcode 0xB8")
        #expect(unit.failedProbeGroups[1].address == "Address 0x60")
        #expect(unit.document?.snapshot?.descriptors?.first?.subunit.title == "Subunit 0x1")
        #expect(try !Self.dashboard().needsDocumentUpdate)
    }

    @Test func capturedDuetLegacyDocumentDecodesWithoutLosingDescriptors() throws {
        let url = URL(fileURLWithPath: #filePath).deletingLastPathComponent().deletingLastPathComponent()
            .appendingPathComponent("documentation/avc-rebuild/fixtures/duet_discovery_6253bee_2026-10-03.json")
        let document = try #require(AvcUnitDocument.decode(Data(contentsOf: url)))
        let unit = AvcUnitDashboard(guid: 0x0003DB0A0000D112, nodeID: 1, vendorID: 0x0003DB, modelID: 0x01DDDD, vendorName: "Apogee", modelName: "Duet", deviceState: "ready", wireUnit: nil, document: document)
        #expect(unit.needsDocumentUpdate)
        #expect(unit.sampleRate == 44100)
        let descriptors = try #require(document.snapshot?.descriptors)
        #expect(descriptors.map(\.bytes) == [0, 56, 464, 0])
        #expect(Set(descriptors.map(\.id)).count == descriptors.count)
        #expect(unit.failedProbeGroups.count > 1)
        #expect(unit.failedProbeGroups.allSatisfy { $0.address != "?" && $0.opcode != "?" })
        #expect(document.snapshot?.features?.count == 6)
        #expect(unit.subunits.map { $0.ref.title } == ["Subunit 0x1", "Subunit 0xc"])
    }

    @Test func graphRepliesKeepRejectedAndNotImplementedSeparate() throws {
        var json = try #require(JSONSerialization.jsonObject(with: Self.fixtureData()) as? [String: Any])
        var graph = try #require(json["graph"] as? [String: Any])
        graph["probeResults"] = [
            ["address": 255, "opcode": 26, "responseCode": 8, "responseName": "NOT IMPLEMENTED(0x8)", "error": ["kind": "unexpectedResponse", "response": 8]],
            ["address": 255, "opcode": 26, "responseCode": 10, "responseName": "REJECTED(0xa)", "error": ["kind": "unexpectedResponse", "response": 10]],
            ["address": 255, "opcode": 26, "responseCode": 8, "responseName": "NOT IMPLEMENTED(0x8)", "error": ["kind": "unexpectedResponse", "response": 8]]
        ]
        json["graph"] = graph
        let document = try #require(AvcUnitDocument.decode(JSONSerialization.data(withJSONObject: json)))
        let unit = AvcUnitDashboard(guid: 1, nodeID: 1, vendorID: 0, modelID: 0, vendorName: nil, modelName: nil, deviceState: nil, wireUnit: nil, document: document)
        #expect(unit.failedProbeGroups.count == 2)
        #expect(unit.failedProbeGroups[0].error == "NOT IMPLEMENTED(0x8)")
        #expect(unit.failedProbeGroups[0].count == 2)
        #expect(unit.failedProbeGroups[1].error == "REJECTED(0xa)")
    }

    @Test func everyTabRenders() throws {
        let unit = try Self.dashboard()
        let directory = ProcessInfo.processInfo.environment["ASFW_RENDER_DIR"].map { URL(fileURLWithPath: $0) }
            ?? FileManager.default.temporaryDirectory.appendingPathComponent("asfw-avc-units")
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        for tab in AvcUnitTab.allCases {
            let view = AvcUnitDashboardView(unit: unit, tab: .constant(tab), developerTools: nil)
                .padding(24)
                .frame(width: 1120)
                .background(Color(nsColor: .windowBackgroundColor))
                .environment(\.colorScheme, .dark)
            let renderer = ImageRenderer(content: view)
            renderer.scale = 2
            renderer.proposedSize = .init(width: 1120, height: nil)
            let image = try #require(renderer.nsImage)
            let tiff = try #require(image.tiffRepresentation)
            let png = try #require(NSBitmapImageRep(data: tiff)?.representation(using: .png, properties: [:]))
            #expect(png.count > 5_000)
            try png.write(to: directory.appendingPathComponent("avc-units-\(tab.rawValue.lowercased()).png"))
        }
    }
}
