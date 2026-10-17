import AppKit
import Foundation
import SwiftUI
import Testing
@testable import ASFW

/// The console layout is derived from the published topology: these pin how a
/// real Duet document and an AV/C feature block fall into strips.
@MainActor
struct ControlConsoleLayoutTests {
    /// The driver's own document for an Apogee Duet (DuetControlModelTests,
    /// DocumentMapsEveryMixerCrosspointToItsPorts, dumped with ASFW_DUMP_DUET_CONTROLS).
    static let duet = Data(#"""
{"schema":"asfw.controls.v1","guid":"0x0003db0a0000d112","family":"duet","revision":7,"sequence":0,"nodes":[{"id":1,"name":"Host Capture","kind":"endpoint","host":true},{"id":2,"name":"Host Playback","kind":"endpoint","host":true},{"id":10,"name":"Analog Input 1","kind":"endpoint","host":false},{"id":1146425344,"name":"Input 1","kind":"router","inputs":[3,4],"outputs":[5],"writable":true,"bundles":[{"id":1,"routes":[[3,5]]},{"id":2,"routes":[[4,5]]}],"activeBundles":[1]},{"id":20,"name":"Gain 1","kind":"processor","inputs":[6],"outputs":[7]},{"id":11,"name":"Analog Input 2","kind":"endpoint","host":false},{"id":1146425345,"name":"Input 2","kind":"router","inputs":[12,13],"outputs":[14],"writable":true,"bundles":[{"id":1,"routes":[[12,14]]},{"id":2,"routes":[[13,14]]}],"activeBundles":[1]},{"id":21,"name":"Gain 2","kind":"processor","inputs":[15],"outputs":[16]},{"id":30,"name":"Monitor Mixer","kind":"mixer","inputs":[19,20,21,22],"outputs":[23,24],"crosspoints":[{"id":1,"in":19,"out":23},{"id":2,"in":20,"out":23},{"id":3,"in":21,"out":23},{"id":4,"in":22,"out":23},{"id":5,"in":19,"out":24},{"id":6,"in":20,"out":24},{"id":7,"in":21,"out":24},{"id":8,"in":22,"out":24}]},{"id":42,"name":"Analog Output","kind":"endpoint","host":false},{"id":40,"name":"Output Source","kind":"router","inputs":[25,26,31,32],"outputs":[27,33],"writable":false,"bundles":[{"id":1,"routes":[[25,27],[31,33]]},{"id":2,"routes":[[26,27],[32,33]]}],"activeBundles":[1]},{"id":41,"name":"Output","kind":"processor","inputs":[28,34],"outputs":[29,35]}],"ports":[{"id":1,"owner":10,"direction":"out","channels":1,"name":"XLR","signalKind":2,"signalIndex":1},{"id":2,"owner":10,"direction":"out","channels":1,"name":"Instrument","signalKind":3,"signalIndex":1},{"id":3,"owner":1146425344,"direction":"in","channels":1,"name":"XLR","signalKind":0,"signalIndex":1},{"id":4,"owner":1146425344,"direction":"in","channels":1,"name":"Jack","signalKind":0,"signalIndex":1},{"id":5,"owner":1146425344,"direction":"out","channels":1,"name":"Selected","signalKind":0,"signalIndex":1},{"id":6,"owner":20,"direction":"in","channels":1,"name":"In","signalKind":0,"signalIndex":1},{"id":7,"owner":20,"direction":"out","channels":1,"name":"Out","signalKind":0,"signalIndex":1},{"id":8,"owner":1,"direction":"in","channels":1,"name":"Capture","signalKind":8,"signalIndex":1},{"id":9,"owner":2,"direction":"out","channels":1,"name":"Playback","signalKind":8,"signalIndex":1},{"id":10,"owner":11,"direction":"out","channels":1,"name":"XLR","signalKind":2,"signalIndex":2},{"id":11,"owner":11,"direction":"out","channels":1,"name":"Instrument","signalKind":3,"signalIndex":2},{"id":12,"owner":1146425345,"direction":"in","channels":1,"name":"XLR","signalKind":0,"signalIndex":1},{"id":13,"owner":1146425345,"direction":"in","channels":1,"name":"Jack","signalKind":0,"signalIndex":1},{"id":14,"owner":1146425345,"direction":"out","channels":1,"name":"Selected","signalKind":0,"signalIndex":1},{"id":15,"owner":21,"direction":"in","channels":1,"name":"In","signalKind":0,"signalIndex":1},{"id":16,"owner":21,"direction":"out","channels":1,"name":"Out","signalKind":0,"signalIndex":1},{"id":17,"owner":1,"direction":"in","channels":1,"name":"Capture","signalKind":8,"signalIndex":2},{"id":18,"owner":2,"direction":"out","channels":1,"name":"Playback","signalKind":8,"signalIndex":2},{"id":19,"owner":30,"direction":"in","channels":1,"name":"Analog","signalKind":0,"signalIndex":1},{"id":20,"owner":30,"direction":"in","channels":1,"name":"Analog","signalKind":0,"signalIndex":1},{"id":21,"owner":30,"direction":"in","channels":1,"name":"Playback","signalKind":0,"signalIndex":1},{"id":22,"owner":30,"direction":"in","channels":1,"name":"Playback","signalKind":0,"signalIndex":1},{"id":23,"owner":30,"direction":"out","channels":1,"name":"Mix","signalKind":0,"signalIndex":1},{"id":24,"owner":30,"direction":"out","channels":1,"name":"Mix","signalKind":0,"signalIndex":1},{"id":25,"owner":40,"direction":"in","channels":1,"name":"Stream","signalKind":0,"signalIndex":1},{"id":26,"owner":40,"direction":"in","channels":1,"name":"Mixer","signalKind":0,"signalIndex":1},{"id":27,"owner":40,"direction":"out","channels":1,"name":"Selected","signalKind":0,"signalIndex":1},{"id":28,"owner":41,"direction":"in","channels":1,"name":"In","signalKind":0,"signalIndex":1},{"id":29,"owner":41,"direction":"out","channels":1,"name":"Out","signalKind":0,"signalIndex":1},{"id":30,"owner":42,"direction":"in","channels":1,"name":"Line","signalKind":1,"signalIndex":1},{"id":31,"owner":40,"direction":"in","channels":1,"name":"Stream","signalKind":0,"signalIndex":1},{"id":32,"owner":40,"direction":"in","channels":1,"name":"Mixer","signalKind":0,"signalIndex":1},{"id":33,"owner":40,"direction":"out","channels":1,"name":"Selected","signalKind":0,"signalIndex":1},{"id":34,"owner":41,"direction":"in","channels":1,"name":"In","signalKind":0,"signalIndex":1},{"id":35,"owner":41,"direction":"out","channels":1,"name":"Out","signalKind":0,"signalIndex":1},{"id":36,"owner":42,"direction":"in","channels":1,"name":"Line","signalKind":1,"signalIndex":2}],"fixedLinks":[[1,3],[2,4],[5,6],[7,8],[10,12],[11,13],[14,15],[16,17],[7,19],[16,20],[9,21],[18,22],[9,25],[23,26],[27,28],[29,30],[18,31],[24,32],[33,34],[35,36]],"parameters":[{"id":1146421264,"name":"Gain 1","semantic":"level","target":{"port":7},"access":"readWrite","domain":{"kind":"scalar","min":10,"max":75,"step":1,"unit":"dB"},"applicable":true,"domainOverride":{"kind":"scalar","min":10,"max":75,"step":1,"unit":"dB"},"value":10,"origin":"observed"},{"id":1146421280,"name":"Phantom 1","semantic":"phantomPower","target":{"port":1},"access":"readWrite","domain":{"kind":"bool"},"applicable":true,"value":false,"origin":"observed"},{"id":1146421296,"name":"Polarity 1","semantic":"phaseInvert","target":{"port":1},"access":"readWrite","domain":{"kind":"bool"},"applicable":true,"value":false,"origin":"observed"},{"id":1146421265,"name":"Gain 2","semantic":"level","target":{"port":16},"access":"readWrite","domain":{"kind":"scalar","min":10,"max":75,"step":1,"unit":"dB"},"applicable":true,"domainOverride":{"kind":"scalar","min":10,"max":75,"step":1,"unit":"dB"},"value":75,"origin":"observed"},{"id":1146421281,"name":"Phantom 2","semantic":"phantomPower","target":{"port":10},"access":"readWrite","domain":{"kind":"bool"},"applicable":true,"value":false,"origin":"observed"},{"id":1146421297,"name":"Polarity 2","semantic":"phaseInvert","target":{"port":10},"access":"readWrite","domain":{"kind":"bool"},"applicable":true,"value":false,"origin":"observed"},{"id":1146421760,"name":"Mixer 1 \u2192 1","semantic":"level","target":{"crosspoint":1},"access":"readWrite","domain":{"kind":"scalar","min":0,"max":16383,"step":1,"unit":"generic"},"applicable":true,"value":0,"origin":"observed"},{"id":1146421761,"name":"Mixer 2 \u2192 1","semantic":"level","target":{"crosspoint":2},"access":"readWrite","domain":{"kind":"scalar","min":0,"max":16383,"step":1,"unit":"generic"},"applicable":true,"value":0,"origin":"observed"},{"id":1146421762,"name":"Mixer 3 \u2192 1","semantic":"level","target":{"crosspoint":3},"access":"readWrite","domain":{"kind":"scalar","min":0,"max":16383,"step":1,"unit":"generic"},"applicable":true,"value":0,"origin":"observed"},{"id":1146421763,"name":"Mixer 4 \u2192 1","semantic":"level","target":{"crosspoint":4},"access":"readWrite","domain":{"kind":"scalar","min":0,"max":16383,"step":1,"unit":"generic"},"applicable":true,"value":0,"origin":"observed"},{"id":1146421764,"name":"Mixer 1 \u2192 2","semantic":"level","target":{"crosspoint":5},"access":"readWrite","domain":{"kind":"scalar","min":0,"max":16383,"step":1,"unit":"generic"},"applicable":true,"value":0,"origin":"observed"},{"id":1146421765,"name":"Mixer 2 \u2192 2","semantic":"level","target":{"crosspoint":6},"access":"readWrite","domain":{"kind":"scalar","min":0,"max":16383,"step":1,"unit":"generic"},"applicable":true,"value":0,"origin":"observed"},{"id":1146421766,"name":"Mixer 3 \u2192 2","semantic":"level","target":{"crosspoint":7},"access":"readWrite","domain":{"kind":"scalar","min":0,"max":16383,"step":1,"unit":"generic"},"applicable":true,"value":0,"origin":"observed"},{"id":1146421767,"name":"Mixer 4 \u2192 2","semantic":"level","target":{"crosspoint":8},"access":"readWrite","domain":{"kind":"scalar","min":0,"max":16383,"step":1,"unit":"generic"},"applicable":true,"value":0,"origin":"observed"},{"id":1146421504,"name":"Output Volume","semantic":"level","target":{"node":41},"access":"readWrite","domain":{"kind":"scalar","min":-64,"max":0,"step":1,"unit":"dB"},"applicable":true,"value":-0,"origin":"observed"},{"id":1146421505,"name":"Output Mute","semantic":"mute","target":{"node":41},"access":"readWrite","domain":{"kind":"bool"},"applicable":true,"value":false,"origin":"observed"},{"id":1146421506,"name":"Output Source","semantic":"source","target":{"node":40},"access":"readWrite","domain":{"kind":"enum","values":[{"value":0,"name":"Stream"},{"value":1,"name":"Mixer"}]},"applicable":true,"value":0,"origin":"observed"},{"id":1146421507,"name":"Output Level","semantic":"nominalLevel","target":{"node":41},"access":"readWrite","domain":{"kind":"enum","values":[{"value":0,"name":"Instrument"},{"value":1,"name":"-10 dBV"}]},"applicable":true,"value":0,"origin":"observed"},{"id":1146421508,"name":"Line Mute Mode","semantic":"unknown","target":{"node":41},"access":"readWrite","domain":{"kind":"enum","values":[{"value":0,"name":"Never"},{"value":1,"name":"Normal"},{"value":2,"name":"Swapped"}]},"applicable":true,"value":0,"origin":"observed"},{"id":1146421509,"name":"Headphone Mute Mode","semantic":"unknown","target":{"node":41},"access":"readWrite","domain":{"kind":"enum","values":[{"value":0,"name":"Never"},{"value":1,"name":"Normal"},{"value":2,"name":"Swapped"}]},"applicable":true,"value":0,"origin":"observed"},{"id":1146421510,"name":"Clickless","semantic":"unknown","target":{"node":1},"access":"readWrite","domain":{"kind":"bool"},"applicable":true,"value":false,"origin":"observed"},{"id":1146421511,"name":"Knob Target","semantic":"unknown","target":{"node":1},"access":"readWrite","domain":{"kind":"enum","values":[{"value":0,"name":"Output"},{"value":1,"name":"Input 1"},{"value":2,"name":"Input 2"}]},"applicable":true,"value":0,"origin":"observed"},{"id":1146421512,"name":"Display Target","semantic":"unknown","target":{"node":1},"access":"readWrite","domain":{"kind":"enum","values":[{"value":0,"name":"Output"},{"value":1,"name":"Input"}]},"applicable":true,"value":0,"origin":"observed"},{"id":1146421513,"name":"Display Mode","semantic":"unknown","target":{"node":1},"access":"readWrite","domain":{"kind":"enum","values":[{"value":0,"name":"Independent"},{"value":1,"name":"Follow Knob"}]},"applicable":true,"value":0,"origin":"observed"},{"id":1146421514,"name":"Display Overhold","semantic":"unknown","target":{"node":1},"access":"readWrite","domain":{"kind":"enum","values":[{"value":0,"name":"Infinite"},{"value":1,"name":"Two Seconds"}]},"applicable":true,"value":0,"origin":"observed"},{"id":1146421515,"name":"Clear Display","semantic":"unknown","target":{"node":1},"access":"readWrite","domain":{"kind":"action"},"applicable":true,"value":null,"origin":null},{"id":3293908992,"name":"Input Type","semantic":"nominalLevel","target":{"node":1146425344},"domain":{"kind":"enum","values":[{"value":0,"name":"Mic"},{"value":1,"name":"+4 dBu"},{"value":2,"name":"-10 dBV"},{"value":3,"name":"Inst"}]},"access":"readWrite","applicable":true,"value":0,"origin":"derived"},{"id":3293908993,"name":"Input Type","semantic":"nominalLevel","target":{"node":1146425345},"domain":{"kind":"enum","values":[{"value":0,"name":"Mic"},{"value":1,"name":"+4 dBu"},{"value":2,"name":"-10 dBV"},{"value":3,"name":"Inst"}]},"access":"readWrite","applicable":true,"value":0,"origin":"derived"}]}
"""#.utf8)

    /// An AV/C feature block on two host playback channels, as the AV/C
    /// projection publishes it: master on the node, channels on output ports.
    static let avcFeature = Data(#"""
    {"schema":"asfw.controls.v1","guid":"0x000aac0300b1d1f7","family":"avc","revision":3,"sequence":0,
     "nodes":[
      {"id":66817,"name":"Master Feature","kind":"processor","inputs":[1,3],"outputs":[2,4]},
      {"id":268435713,"name":"Host Playback","kind":"endpoint","host":true}],
     "ports":[
      {"id":1,"owner":66817,"direction":"in","channels":1,"name":"Ch 1 In","signalKind":0,"signalIndex":1},
      {"id":2,"owner":66817,"direction":"out","channels":1,"name":"Ch 1 Out","signalKind":0,"signalIndex":1},
      {"id":3,"owner":66817,"direction":"in","channels":1,"name":"Ch 2 In","signalKind":0,"signalIndex":1},
      {"id":4,"owner":66817,"direction":"out","channels":1,"name":"Ch 2 Out","signalKind":0,"signalIndex":1},
      {"id":5,"owner":268435713,"direction":"out","channels":1,"name":"Playback 1","signalKind":8,"signalIndex":1},
      {"id":6,"owner":268435713,"direction":"out","channels":1,"name":"Playback 2","signalKind":8,"signalIndex":2}],
     "fixedLinks":[[5,1],[6,3]],
     "parameters":[
      {"id":100,"name":"Master Volume","semantic":"level","target":{"node":66817},
       "domain":{"kind":"scalar","min":-60.0000,"max":6.0000,"step":0.5000,"unit":"dB"},"value":-10.5000,"origin":"observed"},
      {"id":101,"name":"Master Mute","semantic":"mute","target":{"node":66817},"domain":{"kind":"bool"},"value":false,"origin":"observed"},
      {"id":102,"name":"Ch 1 Volume","semantic":"level","target":{"port":2},
       "domain":{"kind":"scalar","min":-60.0000,"max":6.0000,"step":0.5000,"unit":"dB"},"value":-3.0000,"origin":"observed"},
      {"id":103,"name":"Ch 2 Volume","semantic":"level","target":{"port":4},
       "domain":{"kind":"scalar","min":-60.0000,"max":6.0000,"step":0.5000,"unit":"dB"},"value":-3.0000,"origin":"observed"}]}
    """#.utf8)

    private func names(_ snapshot: ControlSnapshot, _ ids: [UInt32]) -> [String] {
        ids.compactMap { id in snapshot.parameters.first { $0.id == id }?.name }
    }

    @Test func duetInputsAreOneStripPerChannelWithEverythingOnTheirPath() throws {
        let snapshot = try ControlSnapshot.decode(Self.duet)
        let layout = ControlConsoleLayout(snapshot)
        #expect(layout.inputs.map(\.title) == ["Analog Input 1", "Analog Input 2"])
        #expect(names(snapshot, layout.inputs[0].parameters) == ["Gain 1", "Phantom 1", "Polarity 1", "Input Type"])
        #expect(names(snapshot, layout.inputs[1].parameters) == ["Gain 2", "Phantom 2", "Polarity 2", "Input Type"])
        #expect(layout.inputs.allSatisfy { $0.channels == 1 && $0.routers.isEmpty })
    }

    @Test func duetOutputIsOneStereoStripWithItsSourceSelector() throws {
        let snapshot = try ControlSnapshot.decode(Self.duet)
        let layout = ControlConsoleLayout(snapshot)
        try #require(layout.outputs.count == 1)
        let output = layout.outputs[0]
        #expect(output.title == "Analog Output")
        #expect(output.channels == 2)
        #expect(names(snapshot, output.parameters) == ["Output Volume", "Output Mute", "Output Source", "Output Level",
                                                       "Line Mute Mode", "Headphone Mute Mode"])
        // Output Source speaks for its router: no second route menu.
        #expect(output.routers.isEmpty)
        #expect(layout.other.isEmpty)
    }

    @Test func duetMixerHasOneStripPerSourceWithLeftAndRightSends() throws {
        let snapshot = try ControlSnapshot.decode(Self.duet)
        let layout = ControlConsoleLayout(snapshot)
        try #require(layout.mixers.count == 1)
        let mixer = layout.mixers[0]
        #expect(mixer.title == "Monitor Mixer")
        #expect(mixer.sources.map(\.title) == ["Analog Input 1", "Analog Input 2", "Playback 1", "Playback 2"])
        #expect(mixer.sources.allSatisfy { $0.sends.map(\.label) == ["L", "R"] })
        #expect(mixer.sources[0].sends.map(\.destination) == ["Analog Output 1", "Analog Output 2"])
        // Source k's send to output o is crosspoint o*4+k+1, parameter Mixer base + o*4 + k.
        #expect(names(snapshot, mixer.sources[2].sends.map(\.parameter)) == ["Mixer 3 → 1", "Mixer 3 → 2"])
    }

    @Test func duetDeviceWideSettingsAreLeftForTheSettingsCard() throws {
        let snapshot = try ControlSnapshot.decode(Self.duet)
        let layout = ControlConsoleLayout(snapshot)
        #expect(names(snapshot, layout.settings) == ["Clickless", "Knob Target", "Display Target", "Display Mode",
                                                     "Display Overhold", "Clear Display"])
        let placed = (layout.inputs + layout.outputs).flatMap(\.parameters)
            + layout.mixers.flatMap { $0.sources.flatMap { $0.sends.map(\.parameter) } } + layout.settings
        #expect(placed.sorted() == snapshot.parameters.map(\.id).sorted())
    }

    @Test func avcFeatureGivesChannelStripsThenTheMaster() throws {
        let snapshot = try ControlSnapshot.decode(Self.avcFeature)
        let layout = ControlConsoleLayout(snapshot)
        #expect(layout.inputs.isEmpty)
        #expect(layout.outputs.map(\.title) == ["Playback 1", "Playback 2", "Master Feature"])
        #expect(layout.outputs.map(\.channels) == [1, 1, 2])
        #expect(names(snapshot, layout.outputs[2].parameters) == ["Master Volume", "Master Mute"])
        #expect(layout.settings.isEmpty)
    }

    @Test func aDocumentWithoutLinksStillPlacesEveryControl() throws {
        // Older drivers: no fixedLinks, no crosspoint map. Channels are named by
        // their port, and crosspoint parameters stay in the settings card.
        let snapshot = try ControlSnapshot.decode(ControlSnapshotTests.document())
        let layout = ControlConsoleLayout(snapshot)
        #expect(layout.mixers.isEmpty)
        #expect(layout.other.map(\.title) == ["Ch 1", "Ch 2", "Master Feature"])
        #expect(names(snapshot, layout.settings) == ["Pan"])
    }

    @Test func aWritableRouterWithoutAParameterGetsARouteMenu() throws {
        var object = try #require(JSONSerialization.jsonObject(with: ControlSnapshotTests.document()) as? [String: Any])
        var nodes = try #require(object["nodes"] as? [[String: Any]])
        nodes[1]["writable"] = true
        object["nodes"] = nodes
        let snapshot = try ControlSnapshot.decode(try JSONSerialization.data(withJSONObject: object))
        let layout = ControlConsoleLayout(snapshot)
        let strip = try #require(layout.other.first { $0.routers == [66561] })
        #expect(strip.parameters.isEmpty)
        #expect(strip.title == "Clock Selector")
    }
}

/// Renders the console for the Duet document to PNGs so the layout can be
/// looked at (set ASFW_RENDER_DIR to keep them). The fake driver serves it.
@MainActor
struct ControlConsoleRenderTests {
    @Test func rendersTheDuetConsole() async throws {
        let driver = FakeControlsDriver()
        driver.documentOverride = Data(String(decoding: ControlConsoleLayoutTests.duet, as: UTF8.self)
            .replacingOccurrences(of: "\"sequence\":0,", with: "").utf8)
        let model = DeviceControlsViewModel(source: driver)
        model.reload()
        let snapshot = try #require(model.snapshot)
        #expect(snapshot.family == "duet")

        let directory = ProcessInfo.processInfo.environment["ASFW_RENDER_DIR"].map { URL(fileURLWithPath: $0) }
            ?? FileManager.default.temporaryDirectory.appendingPathComponent("asfw-device-controls")
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        for scheme in [ColorScheme.light, .dark] {
            let view = DeviceControlsContent(model: model, presentation: .console, consoleScrolls: false)
                .padding(24)
                .frame(width: 1500)
                .background(Color(nsColor: .windowBackgroundColor))
                .environment(\.colorScheme, scheme)
            let renderer = ImageRenderer(content: view)
            renderer.scale = 2
            renderer.proposedSize = .init(width: 1500, height: nil)
            let image = try #require(renderer.nsImage)
            let tiff = try #require(image.tiffRepresentation)
            let png = try #require(NSBitmapImageRep(data: tiff)?.representation(using: .png, properties: [:]))
            #expect(png.count > 5_000)
            try png.write(to: directory.appendingPathComponent("console-duet-\(scheme == .dark ? "dark" : "light").png"))
        }
    }
}
