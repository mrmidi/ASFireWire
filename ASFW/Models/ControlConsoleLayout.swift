import Foundation

/// A control snapshot arranged as a hardware console: input strips, mixers,
/// output strips and device settings, derived from the topology the driver
/// publishes rather than from a per-device layout.
///
/// A *lane* is one channel's signal path: ports joined by fixed links, router
/// routes and channel-preserving processors. Mixers and endpoints end a lane.
/// Parameters that act on the same set of lanes share a strip, so a channel's
/// gain, phantom power and input type land on one strip, and a node-wide
/// output volume over two lanes becomes one stereo strip.
nonisolated struct ControlConsoleLayout: Equatable, Sendable {
    enum Role: Equatable, Sendable { case input, output, other }

    struct Strip: Identifiable, Equatable, Sendable {
        /// Stable across value changes: the lanes the strip covers.
        let id: String
        let title: String
        let role: Role
        let channels: Int
        /// Parameter ids in document order.
        let parameters: [UInt32]
        /// Writable routers on the strip that no parameter already controls.
        let routers: [UInt32]
    }

    struct Send: Identifiable, Equatable, Sendable {
        var id: UInt32 { parameter }
        let parameter: UInt32
        /// Short fader label ("L", "R" or the output's number).
        let label: String
        /// Where the send goes, for accessibility and help.
        let destination: String
    }

    struct MixerSource: Identifiable, Equatable, Sendable {
        /// The mixer input port.
        let id: UInt32
        let title: String
        let sends: [Send]
    }

    struct Mixer: Identifiable, Equatable, Sendable {
        let id: UInt32
        let title: String
        let sources: [MixerSource]
    }

    var inputs: [Strip] = []
    var mixers: [Mixer] = []
    var outputs: [Strip] = []
    var other: [Strip] = []
    /// Parameters with no place on a strip or mixer: device-wide settings
    /// (they target an endpoint) and anything the layout cannot place.
    var settings: [UInt32] = []

    var isEmpty: Bool { inputs.isEmpty && mixers.isEmpty && outputs.isEmpty && other.isEmpty }

    init(_ snapshot: ControlSnapshot) {
        let graph = Graph(snapshot)
        var placedInMixer = Set<UInt32>()
        mixers = snapshot.nodes.filter { $0.kind == .mixer }.compactMap { node in
            graph.mixer(node, placed: &placedInMixer)
        }

        // Group the remaining parameters by the lanes they act on.
        var groups: [[UInt32]: [UInt32]] = [:]
        var order: [[UInt32]] = []
        for parameter in snapshot.parameters where !placedInMixer.contains(parameter.id) {
            guard let lanes = graph.lanes(of: parameter), !lanes.isEmpty else {
                settings.append(parameter.id)
                continue
            }
            if groups[lanes] == nil { order.append(lanes) }
            groups[lanes, default: []].append(parameter.id)
        }
        // A writable router no parameter speaks for gets a route menu on its strip.
        var routers: [[UInt32]: [UInt32]] = [:]
        for node in snapshot.nodes where node.kind == .router && node.writable && !node.bundles.isEmpty {
            let controlled = snapshot.parameters.contains { $0.target == .node(node.id) }
            let lanes = graph.lanes(ofNode: node.id)
            guard !controlled, !lanes.isEmpty else { continue }
            if groups[lanes] == nil { order.append(lanes); groups[lanes] = [] }
            routers[lanes, default: []].append(node.id)
        }

        let strips = order.map { lanes in
            Strip(id: lanes.map(String.init).joined(separator: ","),
                  title: graph.title(lanes: lanes, parameters: groups[lanes] ?? [], routers: routers[lanes] ?? []),
                  role: graph.role(lanes: lanes),
                  channels: lanes.count,
                  parameters: groups[lanes] ?? [],
                  routers: routers[lanes] ?? [])
        }
        // Per-channel strips first, then the strips spanning several channels
        // (a master), each group in lane order.
        .sorted { ($0.role.rank, $0.channels > 1 ? 1 : 0, graph.rank(of: $0.id))
                < ($1.role.rank, $1.channels > 1 ? 1 : 0, graph.rank(of: $1.id)) }
        inputs = strips.filter { $0.role == .input }
        outputs = strips.filter { $0.role == .output }
        other = strips.filter { $0.role == .other }
    }
}

private extension ControlConsoleLayout.Role {
    var rank: Int {
        switch self {
        case .input: 0
        case .output: 1
        case .other: 2
        }
    }
}

/// The port graph behind the layout.
private nonisolated struct Graph {
    /// Driver SignalKind::HostStream.
    static let hostStreamKind = 8

    let snapshot: ControlSnapshot
    private var parent: [UInt32: UInt32] = [:]
    private var lanesByNode: [UInt32: [UInt32]] = [:]
    private let ports: [UInt32: ControlPort]
    private let nodes: [UInt32: ControlNode]
    private let links: [ControlRoute]

    init(_ snapshot: ControlSnapshot) {
        self.snapshot = snapshot
        ports = Dictionary(snapshot.ports.map { ($0.id, $0) }, uniquingKeysWith: { first, _ in first })
        nodes = Dictionary(snapshot.nodes.map { ($0.id, $0) }, uniquingKeysWith: { first, _ in first })
        links = snapshot.fixedLinks ?? []
        for port in snapshot.ports { parent[port.id] = port.id }
        for link in links { union(link.input, link.output) }
        for node in snapshot.nodes {
            switch node.kind {
            case .router:
                for bundle in node.bundles { for route in bundle.routes { union(route.input, route.output) } }
            case .processor:
                if node.inputs.count == node.outputs.count {
                    for (input, output) in zip(node.inputs, node.outputs) { union(input, output) }
                } else if let first = (node.inputs + node.outputs).first {
                    for port in node.inputs + node.outputs { union(first, port) }
                }
            case .mixer, .endpoint:
                break
            }
        }
        for port in snapshot.ports {
            let lane = root(port.id)
            if !(lanesByNode[port.owner]?.contains(lane) ?? false) { lanesByNode[port.owner, default: []].append(lane) }
        }
        for key in lanesByNode.keys { lanesByNode[key]?.sort() }
    }

    // MARK: Lanes

    func root(_ port: UInt32) -> UInt32 {
        var current = port
        while let next = parent[current], next != current { current = next }
        return current
    }

    private mutating func union(_ a: UInt32, _ b: UInt32) {
        guard parent[a] != nil, parent[b] != nil else { return }
        let (ra, rb) = (root(a), root(b))
        // The lower port id names the lane, so lane ids follow document order.
        if ra < rb { parent[rb] = ra } else if rb < ra { parent[ra] = rb }
    }

    func lanes(ofNode node: UInt32) -> [UInt32] { lanesByNode[node] ?? [] }

    /// The lanes a parameter acts on; nil when it belongs in device settings.
    func lanes(of parameter: ControlParameter) -> [UInt32]? {
        switch parameter.target {
        case .port(let id):
            return ports[id] == nil ? nil : [root(id)]
        case .node(let id):
            guard let node = nodes[id], node.kind != .endpoint, node.kind != .mixer else { return nil }
            return lanes(ofNode: id)
        case .crosspoint:
            return nil
        }
    }

    func rank(of stripId: String) -> UInt32 {
        UInt32(stripId.split(separator: ",").first.flatMap { UInt32($0) } ?? .max)
    }

    private func portsIn(_ lanes: Set<UInt32>) -> [ControlPort] {
        snapshot.ports.filter { lanes.contains(root($0.id)) }
    }

    private func isHost(_ port: ControlPort) -> Bool {
        nodes[port.owner]?.host == true || port.signalKind == Self.hostStreamKind
    }

    func role(lanes: [UInt32]) -> ControlConsoleLayout.Role {
        let endpoints = portsIn(Set(lanes)).filter { nodes[$0.owner]?.kind == .endpoint }
        let hosts = endpoints.filter(isHost)
        if hosts.contains(where: { $0.direction == .input }) { return .input }
        if hosts.contains(where: { $0.direction == .output }) { return .output }
        let physical = endpoints.filter { !isHost($0) }
        if physical.contains(where: { $0.direction == .input }) { return .output }
        if physical.contains(where: { $0.direction == .output }) { return .input }
        return .other
    }

    // MARK: Names

    func title(lanes: [UInt32], parameters: [UInt32], routers: [UInt32]) -> String {
        let set = Set(lanes)
        // A strip that is only a route menu is named after its router.
        if parameters.isEmpty, let router = routers.first.flatMap({ nodes[$0] }) { return router.name }
        // A physical connector the strip covers completely names it ("Analog Input 1").
        let contained = snapshot.nodes.filter { node in
            guard node.kind == .endpoint, node.host != true else { return false }
            let own = self.lanes(ofNode: node.id)
            return !own.isEmpty && own.allSatisfy(set.contains)
        }
        if contained.count == 1 { return contained[0].name }
        if lanes.count == 1 {
            let endpoints = portsIn(set).filter { nodes[$0.owner]?.kind == .endpoint }
            if let port = endpoints.first(where: { !isHost($0) }) ?? endpoints.first { return name(ofEndpointPort: port) }
            // An unconnected channel: the port its controls act on, without the direction ("Ch 1").
            for id in parameters {
                guard case .port(let portId)? = snapshot.parameters.first(where: { $0.id == id })?.target,
                      let port = ports[portId] else { continue }
                for suffix in [" Out", " In"] where port.name.hasSuffix(suffix) {
                    return String(port.name.dropLast(suffix.count))
                }
                return port.name
            }
        }
        // Otherwise the block holding most of the strip's parameters ("Output").
        var counts: [UInt32: Int] = [:]
        for id in parameters {
            guard let parameter = snapshot.parameters.first(where: { $0.id == id }),
                  let owner = snapshot.ownerNode(of: parameter) else { continue }
            counts[owner, default: 0] += 1
        }
        if let owner = counts.max(by: { ($0.value, $1.key) < ($1.value, $0.key) })?.key, let node = nodes[owner] {
            return node.name
        }
        if lanes.count == 1, let port = ports[lanes[0]] { return port.name }
        return "Channels \(lanes.count)"
    }

    /// "Analog Input 1", "Playback 2": the endpoint's name, numbered when the
    /// endpoint carries more than one channel.
    func name(ofEndpointPort port: ControlPort) -> String {
        guard let node = nodes[port.owner] else { return port.name }
        if isHost(port) {
            guard let index = port.signalIndex, port.name.last?.isNumber != true else { return port.name }
            return "\(port.name) \(index)"
        }
        let indices = Set(snapshot.ports.filter { $0.owner == node.id }.compactMap(\.signalIndex))
        if indices.count > 1, let index = port.signalIndex { return "\(node.name) \(index)" }
        return node.name
    }

    /// The endpoint port that feeds `port`, followed back through links,
    /// processors and routers (the active route first). Nil at a mixer.
    func source(of port: UInt32) -> ControlPort? {
        var current = port
        for _ in 0..<64 {
            guard let link = links.first(where: { $0.output == current }), let from = ports[link.input],
                  let owner = nodes[from.owner] else { return nil }
            switch owner.kind {
            case .endpoint:
                return from
            case .processor:
                guard let index = owner.outputs.firstIndex(of: from.id) else { return nil }
                guard let input = owner.inputs.count == owner.outputs.count ? owner.inputs[index] : owner.inputs.first else { return nil }
                current = input
            case .router:
                let bundles = owner.bundles.filter { owner.activeBundles.contains($0.id) } + owner.bundles
                guard let route = bundles.lazy.flatMap(\.routes).first(where: { $0.output == from.id }) else { return nil }
                current = route.input
            case .mixer:
                return nil
            }
        }
        return nil
    }

    /// The endpoint port `port` reaches, followed forward. Nil when none.
    func destination(of port: UInt32) -> ControlPort? {
        var current = port
        for _ in 0..<64 {
            guard let link = links.first(where: { $0.input == current }), let to = ports[link.output],
                  let owner = nodes[to.owner] else { return nil }
            switch owner.kind {
            case .endpoint:
                return to
            case .processor:
                guard let index = owner.inputs.firstIndex(of: to.id) else { return nil }
                guard let output = owner.inputs.count == owner.outputs.count ? owner.outputs[index] : owner.outputs.first else { return nil }
                current = output
            case .router:
                guard let route = owner.bundles.lazy.flatMap(\.routes).first(where: { $0.input == to.id }) else { return nil }
                current = route.output
            case .mixer:
                return nil
            }
        }
        return nil
    }

    // MARK: Mixers

    /// A mixer as one strip per input, one send per output. Nil when the
    /// driver publishes no crosspoint map (an older driver); its parameters
    /// then stay in device settings.
    func mixer(_ node: ControlNode, placed: inout Set<UInt32>) -> ControlConsoleLayout.Mixer? {
        guard !node.crosspoints.isEmpty else { return nil }
        var parameterByCrosspoint: [UInt32: UInt32] = [:]
        for parameter in snapshot.parameters {
            if case .crosspoint(let id) = parameter.target { parameterByCrosspoint[id] = parameter.id }
        }
        let outputLabels: [UInt32: (label: String, destination: String)] = Dictionary(
            node.outputs.enumerated().map { index, port in
                let label = node.outputs.count == 2 ? (index == 0 ? "L" : "R") : "\(index + 1)"
                let destination = destination(of: port).map(name(ofEndpointPort:)) ?? "\(node.name) \(index + 1)"
                return (port, (label, destination))
            }, uniquingKeysWith: { first, _ in first })
        var sources: [ControlConsoleLayout.MixerSource] = []
        for (index, input) in node.inputs.enumerated() {
            let sends = node.outputs.compactMap { output -> ControlConsoleLayout.Send? in
                guard let crosspoint = node.crosspoints.first(where: { $0.input == input && $0.output == output }),
                      let parameter = parameterByCrosspoint[crosspoint.id], let label = outputLabels[output] else { return nil }
                placed.insert(parameter)
                return .init(parameter: parameter, label: label.label, destination: label.destination)
            }
            guard !sends.isEmpty else { continue }
            let title = source(of: input).map(name(ofEndpointPort:)) ?? ports[input]?.name ?? "Input \(index + 1)"
            sources.append(.init(id: input, title: title, sends: sends))
        }
        return sources.isEmpty ? nil : .init(id: node.id, title: node.name, sources: sources)
    }
}
