import Foundation

/// One AV/C unit as the screen shows it: the device's identity, the driver's discovery document, and the
/// groupings the views need (channel rows, feature blocks, routing choices). Pure presentation: every name and
/// value comes from the driver; nothing here knows AV/C.
struct AvcUnitDashboard: Identifiable {
    // MARK: Identity

    let guid: UInt64
    let nodeID: UInt16
    let vendorID: UInt32
    let modelID: UInt32
    let vendorName: String?
    let modelName: String?
    let deviceState: String?
    /// What the unit list reported before the document was read (subunits and plug counts).
    let wireUnit: AVCUnitInfo?
    let document: AvcUnitDocument?

    /// Older documents omit parsed contents and names even when descriptors were read.
    var needsDocumentUpdate: Bool {
        document?.snapshot != nil && document?.snapshot?.contents == nil
    }

    var documentUpdateMessage: String {
        "The running driver reports an older discovery document. Install the current driver and reconnect to show parsed capabilities, controls, and plug details. Re-scanning with the older driver will not add these fields."
    }

    var id: UInt64 { guid }
    var guidHex: String { String(format: "0x%016llX", guid) }
    var title: String {
        if let modelName, !modelName.isEmpty { return modelName }
        return "AV/C unit \(guidHex)"
    }
    var subtitle: String {
        var parts: [String] = []
        if let vendorName, !vendorName.isEmpty { parts.append(vendorName) }
        parts.append("Node \(nodeID)")
        return parts.joined(separator: " · ")
    }

    // MARK: Discovery state

    enum Health: Equatable {
        case complete, partial(String), cancelled, noData

        var label: String {
            switch self {
            case .complete: "Discovered"
            case .partial: "Partial discovery"
            case .cancelled: "Discovery cancelled"
            case .noData: "No discovery data"
            }
        }
    }

    var health: Health {
        guard let snapshot = document?.snapshot else { return .noData }
        if snapshot.cancelled { return .cancelled }
        if snapshot.complete { return .complete }
        return .partial(snapshot.terminalError?.kind ?? "incomplete")
    }

    // MARK: Streams

    var playback: AvcUnitDocument.Stream? { document?.graph?.playback }
    var capture: AvcUnitDocument.Stream? { document?.graph?.capture }

    /// The graph retains the last device rate confirmed by discovery or a successful clock change.
    var sampleRate: Int? {
        return [playback?.rate, capture?.rate].compactMap { $0 }.first { $0 > 0 }
    }

    /// Every rate the streams support, ascending, without duplicates.
    var supportedRates: [Int] {
        Array(Set((playback?.rates ?? []) + (capture?.rates ?? []))).sorted()
    }

    static func rateLabel(_ hz: Int) -> String {
        let khz = Double(hz) / 1000
        return khz.truncatingRemainder(dividingBy: 1) == 0 ? "\(Int(khz)) kHz" : String(format: "%.1f kHz", khz)
    }

    struct ChannelRow: Identifiable, Equatable {
        let id: Int
        let number: Int
        let name: String
    }

    /// The channel names of a stream with the part every name shares ("… PHASE88 FW") removed, so the rows show
    /// what tells channels apart.
    static func channelRows(_ stream: AvcUnitDocument.Stream?) -> [ChannelRow] {
        guard let stream, !stream.channelNames.isEmpty else { return [] }
        let trimmed = trimCommonSuffix(stream.channelNames)
        return trimmed.enumerated().map { ChannelRow(id: $0.offset, number: $0.offset + 1, name: $0.element.isEmpty ? "Channel \($0.offset + 1)" : $0.element) }
    }

    static func trimCommonSuffix(_ names: [String]) -> [String] {
        guard names.count > 1, let first = names.first else { return names }
        var suffix = Substring(first)
        for name in names.dropFirst() {
            while !suffix.isEmpty, !name.hasSuffix(suffix) { suffix = suffix.dropFirst() }
            if suffix.isEmpty { return names }
        }
        // Only cut at a word boundary, and never leave a name empty.
        guard let space = suffix.firstIndex(of: " ") else { return names }
        let cut = String(suffix[space...])
        let result = names.map { String($0.dropLast(cut.count)).trimmingCharacters(in: .whitespaces) }
        return result.contains(where: \.isEmpty) ? names : result
    }

    // MARK: Routing and controls

    struct RoutingItem: Identifiable {
        let id: String
        let title: String
        let inputs: [String]
        let current: Int?
    }

    var routing: [RoutingItem] {
        (document?.snapshot?.selectors ?? []).map { selector in
            RoutingItem(id: selector.id, title: selector.title,
                        inputs: (selector.inputs ?? []).map(\.title), current: selector.current)
        }
    }

    struct ControlChannel: Identifiable {
        let id: Int
        let channel: Int
        let muted: Bool?
        let volumeDb: Double?
        let volumeText: String?
        let volumeIsSilence: Bool
        var isMaster: Bool { channel == 0 }
    }

    struct ControlBlock: Identifiable {
        let id: String
        let block: Int
        let title: String
        let channels: [ControlChannel]
    }

    /// The feature controls the driver read, grouped by function block and channel.
    var confirmedFeatureNames: [String] {
        let confirmed = (document?.snapshot?.features ?? []).filter {
            ($0.attribute == nil || $0.attribute == 0x10) && $0.error == nil && $0.decoded != nil
        }
        return Set(confirmed.compactMap(\.controlName)).sorted()
    }

    var controlBlocks: [ControlBlock] {
        let features = document?.snapshot?.features ?? []
        var order: [String] = []
        var grouped: [String: [AvcUnitDocument.Feature]] = [:]
        for feature in features where feature.attribute == nil || feature.attribute == 0x10 {
            let key = "\(feature.subunit.type)-\(feature.subunit.id)-\(feature.block)"
            if grouped[key] == nil { order.append(key) }
            grouped[key, default: []].append(feature)
        }
        return order.compactMap { key in
            guard let items = grouped[key], let first = items.first else { return nil }
            var channelOrder: [Int] = []
            var byChannel: [Int: [AvcUnitDocument.Feature]] = [:]
            for item in items {
                if byChannel[item.channel] == nil { channelOrder.append(item.channel) }
                byChannel[item.channel, default: []].append(item)
            }
            let channels: [ControlChannel] = channelOrder.sorted().compactMap { channel in
                let entries = byChannel[channel] ?? []
                let mute = entries.first { $0.decoded?.kind == "boolean" && $0.error == nil }?.decoded?.on
                let volume = entries.first { $0.decoded?.kind == "volume" && $0.error == nil }?.decoded
                // A channel the device does not have answers with an error for every control.
                guard mute != nil || volume != nil else { return nil }
                return ControlChannel(id: channel, channel: channel, muted: mute, volumeDb: volume?.db,
                                      volumeText: volume?.text, volumeIsSilence: volume?.negativeInfinity ?? false)
            }
            guard !channels.isEmpty else { return nil }
            let title = first.blockName?.isEmpty == false ? first.blockName! : "Function block #\(first.block)"
            return ControlBlock(id: key, block: first.block, title: title, channels: channels)
        }
        .sorted { $0.block < $1.block }
    }

    // MARK: Subunits and plugs

    var subunits: [AvcUnitDocument.SubunitInfo] { document?.snapshot?.unit?.subunits ?? [] }

    func contents(for subunit: AvcUnitDocument.SubunitRef) -> AvcUnitDocument.Contents? {
        document?.snapshot?.contents?.first { $0.subunit == subunit }
    }

    func plugs(of subunit: AvcUnitDocument.SubunitRef?) -> [AvcUnitDocument.Plug] {
        (document?.snapshot?.plugs ?? []).filter { $0.subunit == subunit }
    }

    var musicStatus: AvcUnitDocument.MusicStatus? {
        document?.snapshot?.contents?.compactMap(\.music).first
    }

    var musicIdentifier: AvcUnitDocument.MusicIdentifier? {
        document?.snapshot?.contents?.compactMap(\.musicIdentifier).first
    }

    var audioBlocks: [AvcUnitDocument.FunctionBlock] {
        document?.snapshot?.contents?.compactMap(\.audio).first?.functionBlocks ?? []
    }

    /// MIDI port labels of the whole unit, in the order the device lists them.
    var midiLabels: [String] {
        (musicStatus?.sourcePlugs ?? []).flatMap { $0.midi?.labels ?? [] }
    }

    // MARK: Diagnostics

    struct ProbeGroup: Identifiable {
        let id: String
        let address: String
        let opcode: String
        let error: String
        let count: Int
    }

    /// Failed probes, one entry per distinct (address, opcode, error) with a count: a list walked past its end fails
    /// the same way many times.
    var failedProbeGroups: [ProbeGroup] {
        var order: [String] = []
        var groups: [String: ProbeGroup] = [:]
        let probes: [AvcUnitDocument.FailedProbe]
        if let results = document?.graph?.probeResults {
            probes = results.compactMap { result in
                guard var error = result.error else { return nil }
                if error.kind == "unexpectedResponse" {
                    error.response = result.responseCode ?? error.response
                    error.responseName = result.responseName ?? error.responseName
                }
                return .init(address: result.address, opcode: result.opcode, addressText: result.addressText,
                             opcodeName: result.opcodeName, error: error)
            }
        } else {
            probes = document?.snapshot?.failedProbes ?? []
        }
        for probe in probes {
            let address = probe.addressText.map { Self.withoutRawValue($0) } ?? probe.address.map { String(format: "Address 0x%02X", $0) } ?? "Address not reported"
            let opcode = probe.opcodeName.map { Self.withoutRawValue($0) } ?? probe.opcode.map { String(format: "Opcode 0x%02X", $0) } ?? "Opcode not reported"
            let key = "\(address)|\(opcode)|\(probe.error.kind)|\(probe.error.response.map(String.init) ?? "none")"
            if let existing = groups[key] {
                groups[key] = ProbeGroup(id: key, address: address, opcode: opcode, error: probe.error.displayText, count: existing.count + 1)
            } else {
                order.append(key)
                groups[key] = ProbeGroup(id: key, address: address, opcode: opcode, error: probe.error.displayText, count: 1)
            }
        }
        return order.compactMap { groups[$0] }
    }

    struct RouteDeviation: Identifiable {
        let id: String
        let text: String
        let plugs: [String]
    }

    /// Plugs whose route status the driver found outside the spec, grouped by what it found: the same departure
    /// repeats on every plug of a subunit.
    var routeDeviations: [RouteDeviation] {
        var order: [String] = []
        var plugsByText: [String: [String]] = [:]
        for plug in document?.snapshot?.plugs ?? [] {
            guard let text = plug.route?.deviations, text != "none", text != "n/a" else { continue }
            if plugsByText[text] == nil { order.append(text) }
            let owner = plug.subunit.map { "\($0.title) " } ?? "Unit "
            plugsByText[text, default: []].append("\(owner)\(plug.isInput ? "in" : "out") \(plug.id)")
        }
        return order.map { RouteDeviation(id: $0, text: $0, plugs: plugsByText[$0] ?? []) }
    }

    /// "unexpectedResponse" → "unexpected response": the driver's error kinds are camel case.
    nonisolated static func humanized(_ camelCase: String) -> String {
        var out = ""
        for character in camelCase {
            if character.isUppercase, !out.isEmpty { out += " " }
            out += character.lowercased()
        }
        return out
    }

    /// An opcode or address name without its trailing raw value: "STREAM FORMAT SUPPORT(0x2f)" → "STREAM FORMAT SUPPORT".
    static func withoutRawValue(_ name: String) -> String {
        guard let open = name.range(of: "(0x", options: .backwards), name.hasSuffix(")") else { return name }
        return String(name[..<open.lowerBound])
    }

    struct LatencySummary {
        let count: Int
        let averageMs: Double
        let maxMs: Double
        let failed: Int
    }

    var latency: LatencySummary? {
        guard let records = document?.exchanges?.records, !records.isEmpty else { return nil }
        let micros = records.map(\.elapsedUs)
        return LatencySummary(count: records.count, averageMs: Double(micros.reduce(0, +)) / Double(micros.count) / 1000,
                              maxMs: Double(micros.max() ?? 0) / 1000, failed: records.filter { $0.outcome != "response" }.count)
    }
}
