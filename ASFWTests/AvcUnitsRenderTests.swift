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
