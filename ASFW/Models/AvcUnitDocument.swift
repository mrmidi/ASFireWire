import Foundation

/// The driver's per-unit discovery document (`asfw.avc.discovery`, version 1), read for display.
///
/// Every field is optional or defaulted: an older driver sends less, and a missing field must show as
/// "not reported", never fail the screen. The app decodes nothing from AV/C here: names ("MUTE_CONTROL",
/// "blocking+non-blocking", "48 kHz"), decoded values (dB, on/off) and spec text all come from the driver.
struct AvcUnitDocument: Decodable, Sendable {
    struct Route: Decodable, Sendable {
        var guid: String?
        var node: Int?
    }

    struct Failure: Decodable, Sendable, Hashable {
        var kind: String
        var response: Int?
    }

    struct SubunitRef: Decodable, Sendable, Hashable {
        var type: Int
        var id: Int
        var typeName: String?
        var title: String { typeName ?? "Subunit 0x\(String(type, radix: 16))" }
    }

    struct SubunitInfo: Decodable, Sendable, Identifiable, Hashable {
        var type: Int
        var id: Int
        var typeName: String?
        var destinationPlugs: Int
        var sourcePlugs: Int
        var plugsDiscovered: Bool?
        var ref: SubunitRef { .init(type: type, id: id, typeName: typeName) }
    }

    struct UnitInfo: Decodable, Sendable {
        var isoInputs: Int
        var isoOutputs: Int
        var externalInputs: Int
        var externalOutputs: Int
        var subunits: [SubunitInfo]
    }

    // MARK: Formats and plugs

    struct Format: Decodable, Sendable, Hashable {
        struct Entry: Decodable, Sendable, Hashable {
            var count: Int
            var format: String?
            var code: Int
        }
        var text: String
        var kind: String
        var rate: String?
        var rateHz: Int?
        var syncSource: Bool?
        var entries: [Entry]?
    }

    struct PlugRoute: Decodable, Sendable {
        var sourceName: String?
        var destinationName: String?
        var status: String?
        /// The route's output_status by name ("ready", "effective", ...).
        var outputStatusName: String?
        var deviations: String?
    }

    struct Plug: Decodable, Sendable, Identifiable {
        var address: Int
        /// The subunit the plug belongs to; nil for a unit plug.
        var subunit: SubunitRef?
        var direction: String
        var id: Int
        var currentDecoded: Format?
        var formationsDecoded: [Format]?
        var route: PlugRoute?
        var isInput: Bool { direction == "input" }
        var isUnitPlug: Bool { subunit == nil }
        /// Whether discovery read anything about this plug beyond its existence.
        var hasDetail: Bool { currentDecoded != nil || route != nil }
        var key: String { "\(address)-\(direction)-\(id)" }
    }

    // MARK: Controls and routing

    struct Decoded: Decodable, Sendable {
        var text: String
        var kind: String
        var on: Bool?
        var valid: Bool?
        var negativeInfinity: Bool?
        var db: Double?
    }

    struct Feature: Decodable, Sendable {
        var subunit: SubunitRef
        var block: Int
        var channel: Int
        var controlName: String?
        var blockName: String?
        var error: Failure?
        var decoded: Decoded?
    }

    struct SelectorInput: Decodable, Sendable, Hashable {
        var typeName: String?
        var id: Int
        var name: String?
        var title: String {
            if let name, !name.isEmpty { return name }
            if let typeName { return typeName == "not connected" ? typeName : "\(typeName) #\(id)" }
            return "Input"
        }
    }

    struct Selector: Decodable, Sendable, Identifiable {
        var subunit: SubunitRef
        var block: Int
        var current: Int?
        var name: String?
        var inputs: [SelectorInput]?
        var id: String { "\(subunit.type)-\(subunit.id)-\(block)" }
        var title: String { name?.isEmpty == false ? name! : "Selector #\(block)" }
        var currentInput: SelectorInput? {
            guard let current, let inputs, inputs.indices.contains(current) else { return nil }
            return inputs[current]
        }
    }

    // MARK: Parsed descriptors

    struct MusicGeneral: Decodable, Sendable {
        var transmitName: String?
        var receiveName: String?
        var latency: Int?
    }

    struct MusicCluster: Decodable, Sendable {
        var name: String
        var portTypeName: String?
        var channels: Int
    }

    struct MusicPlug: Decodable, Sendable, Identifiable {
        var id: Int
        /// Destination and source plugs are numbered separately, so `id` alone is not unique.
        var key: String { "\(destination ? "dest" : "src")-\(id)" }
        var destination: Bool
        var usageName: String?
        var name: String
        var clusters: [MusicCluster]
    }

    struct MidiStreams: Decodable, Sendable {
        var declared: Int
        var labels: [String]
    }

    struct Activity: Decodable, Sendable {
        var smpteTimeCodeName: String?
        var sampleCountName: String?
        var audioSyncName: String?
    }

    struct SourcePlugStreams: Decodable, Sendable, Identifiable {
        var plug: Int
        var audioLabels: [String]
        var midi: MidiStreams?
        var activity: Activity?
        var id: Int { plug }
    }

    struct MusicStatus: Decodable, Sendable {
        var general: MusicGeneral?
        var declaredSourcePlugs: Int?
        var plugs: [MusicPlug]
        var sourcePlugs: [SourcePlugStreams]
    }

    struct IdentifierFormat: Decodable, Sendable, Hashable {
        var maxInputChannels: Int
        var maxOutputChannels: Int
        var fdfName: String?
        var am824LabelName: String?
    }

    struct IdentifierMidi: Decodable, Sendable {
        var versionText: String
        var maxInputPorts: Int
        var maxOutputPorts: Int
    }

    struct MusicIdentifier: Decodable, Sendable {
        var versionText: String
        var capabilityNames: [String]
        var general: MusicGeneral?
        var audio: [IdentifierFormat]?
        var midi: IdentifierMidi?
        var smpteTimeCodeName: String?
        var sampleCountName: String?
        var audioSyncName: String?
    }

    struct AudioSource: Decodable, Sendable, Hashable {
        var kind: String
        var id: Int
        var name: String?
    }

    struct FunctionBlock: Decodable, Sendable, Identifiable {
        var typeName: String?
        var id: Int
        var name: String
        var inputs: [AudioSource]
        var clusterChannels: Int?
        var controls: FeatureControls?
        var subTypeName: String?
        var controlNames: [String]?
        var programmableMixerControls: Int?
        var typeInfoError: String?
        var key: String { "\(typeName ?? "?")-\(id)" }
        var title: String { name.isEmpty ? "\(typeName ?? "Block") #\(id)" : name }
    }

    struct FeatureControls: Decodable, Sendable {
        var masterNames: [String]
    }

    struct AudioContents: Decodable, Sendable {
        var functionBlocks: [FunctionBlock]
    }

    struct Contents: Decodable, Sendable {
        var subunit: SubunitRef
        var music: MusicStatus?
        var musicIdentifier: MusicIdentifier?
        var audio: AudioContents?
    }

    // MARK: Discovery bookkeeping

    struct DescriptorRead: Decodable, Sendable, Identifiable {
        var subunit: SubunitRef
        var specifier: String?
        var specifierText: String?
        var bytes: Int
        /// The descriptor as the device sent it, hex.
        var data: String?
        var primaryError: Failure?
        var parseError: ParseFailure?
        var id: String { "\(subunit.type)-\(subunit.id)-\(specifier ?? specifierText ?? "?")" }
    }

    struct ParseFailure: Decodable, Sendable { var kind: String; var offset: Int }

    struct FailedProbe: Decodable, Sendable {
        var address: Int?
        var opcode: Int?
        var addressText: String?
        var opcodeName: String?
        var error: Failure
    }

    struct Snapshot: Decodable, Sendable {
        var complete: Bool
        var cancelled: Bool
        var terminalError: Failure?
        var unit: UnitInfo?
        var plugs: [Plug]?
        var features: [Feature]?
        var selectors: [Selector]?
        var contents: [Contents]?
        var descriptors: [DescriptorRead]?
        var failedProbes: [FailedProbe]?
        var probeCount: Int?
    }

    // MARK: Graph

    struct Stream: Decodable, Sendable {
        var channels: Int
        var midi: Int
        var rate: Int
        var rates: [Int]
        var dataBlockSize: Int?
        var channelNames: [String]
    }

    struct ClockSource: Decodable, Sendable { var name: String; var current: Bool }

    struct Graph: Decodable, Sendable {
        var playback: Stream
        var capture: Stream
        var clockSources: [ClockSource]?
    }

    struct Exchange: Decodable, Sendable {
        var outcome: String
        var elapsedUs: Int
        var retries: Int?
    }

    struct Exchanges: Decodable, Sendable {
        var dropped: Int
        var records: [Exchange]
    }

    var format: String
    var version: Int
    var session: Int
    var route: Route?
    var snapshot: Snapshot?
    var graph: Graph?
    var exchanges: Exchanges?

    static func decode(_ data: Data) -> AvcUnitDocument? {
        guard let document = try? JSONDecoder().decode(AvcUnitDocument.self, from: data),
              document.format == AvcDiscoveryDocument.format, document.version == AvcDiscoveryDocument.supportedVersion else {
            return nil
        }
        return document
    }
}
