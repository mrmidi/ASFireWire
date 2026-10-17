import Foundation

/// One device's control topology and state, schema "asfw.controls.v1"
/// (driver: UserClient/WireFormats/ControlSnapshotDocument.hpp).
///
/// The driver serves it in the same paged envelope as the AV/C discovery
/// document. `revision` names the discovery the parameter ids belong to: a
/// write must quote it, and the driver refuses a write that quotes an older one.
nonisolated struct ControlSnapshot: Decodable, Sendable, Equatable {
    static let schemaName = "asfw.controls.v1"

    var schema: String
    /// "0x" + 16 hex digits.
    var guid: String
    /// The family whose projection produced the document ("avc" today).
    var family: String
    var revision: UInt64
    /// Latest confirmed change included in this snapshot (older drivers omit it).
    var sequence: UInt64?
    var nodes: [ControlNode]
    var ports: [ControlPort]
    var parameters: [ControlParameter]

    static func decode(_ data: Data) throws -> ControlSnapshot {
        let snapshot = try JSONDecoder().decode(ControlSnapshot.self, from: data)
        guard snapshot.schema == schemaName else {
            throw DecodingError.dataCorrupted(.init(codingPath: [CodingKeys.schema],
                                                    debugDescription: "Unsupported schema \(snapshot.schema)"))
        }
        return snapshot
    }

    func port(_ id: UInt32) -> ControlPort? { ports.first { $0.id == id } }
    func node(_ id: UInt32) -> ControlNode? { nodes.first { $0.id == id } }

    /// The node a parameter belongs to: its target node, or the owner of its
    /// target port. A crosspoint parameter has no single node.
    func ownerNode(of parameter: ControlParameter) -> UInt32? {
        switch parameter.target {
        case .node(let id): id
        case .port(let id): port(id)?.owner
        case .crosspoint: nil
        }
    }

    /// The parameters that belong to `node`, node-wide ones (the master) first.
    func parameters(of node: UInt32) -> [ControlParameter] {
        let owned = parameters.filter { ownerNode(of: $0) == node }
        return owned.filter { $0.target.isNode } + owned.filter { !$0.target.isNode }
    }

    /// Parameters no node owns (crosspoints, or a port the document does not list).
    var unownedParameters: [ControlParameter] {
        parameters.filter { parameter in ownerNode(of: parameter).map { node($0) == nil } ?? true }
    }
}

nonisolated struct ControlNode: Decodable, Sendable, Equatable, Identifiable {
    enum Kind: String, Decodable, Sendable, CaseIterable {
        case processor, mixer, router, endpoint
    }

    var id: UInt32
    var name: String
    var kind: Kind
    /// Endpoints carry neither list.
    var inputs: [UInt32]
    var outputs: [UInt32]
    /// Endpoints only: true for the host side.
    var host: Bool?
    /// Routers only: the legal route bundles and the ones active now.
    var bundles: [ControlBundle]
    var activeBundles: [UInt32]
    var writable: Bool = false

    private enum CodingKeys: String, CodingKey { case id, name, kind, inputs, outputs, host, bundles, activeBundles, writable }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        id = try c.decode(UInt32.self, forKey: .id)
        name = try c.decode(String.self, forKey: .name)
        kind = try c.decode(Kind.self, forKey: .kind)
        inputs = try c.decodeIfPresent([UInt32].self, forKey: .inputs) ?? []
        outputs = try c.decodeIfPresent([UInt32].self, forKey: .outputs) ?? []
        host = try c.decodeIfPresent(Bool.self, forKey: .host)
        bundles = try c.decodeIfPresent([ControlBundle].self, forKey: .bundles) ?? []
        activeBundles = try c.decodeIfPresent([UInt32].self, forKey: .activeBundles) ?? []
        writable = try c.decodeIfPresent(Bool.self, forKey: .writable) ?? false
    }

    init(id: UInt32, name: String, kind: Kind, inputs: [UInt32] = [], outputs: [UInt32] = [], host: Bool? = nil,
         bundles: [ControlBundle] = [], activeBundles: [UInt32] = []) {
        self.id = id
        self.name = name
        self.kind = kind
        self.inputs = inputs
        self.outputs = outputs
        self.host = host
        self.bundles = bundles
        self.activeBundles = activeBundles
    }
}

/// A set of routes a router can have active together.
nonisolated struct ControlBundle: Decodable, Sendable, Equatable, Identifiable {
    var id: UInt32
    var routes: [ControlRoute]
}

/// One input port routed to one output port; on the wire `[in, out]`.
nonisolated struct ControlRoute: Decodable, Sendable, Equatable {
    var input: UInt32
    var output: UInt32

    init(input: UInt32, output: UInt32) {
        self.input = input
        self.output = output
    }

    init(from decoder: Decoder) throws {
        var c = try decoder.unkeyedContainer()
        input = try c.decode(UInt32.self)
        output = try c.decode(UInt32.self)
        guard c.isAtEnd else {
            throw DecodingError.dataCorruptedError(in: c, debugDescription: "A route is [input, output]")
        }
    }
}

nonisolated struct ControlPort: Decodable, Sendable, Equatable, Identifiable {
    enum Direction: String, Decodable, Sendable { case input = "in", output = "out" }

    var id: UInt32
    var owner: UInt32
    var direction: Direction
    var channels: Int
    var name: String
}

/// What a parameter means. Unknown names from a newer driver decode as `.unknown`.
nonisolated enum ControlSemantic: String, Decodable, Sendable {
    case level, mute, phantomPower, phaseInvert, pan, balance, solo, nominalLevel, clockSource, dim, source, highPassFilter, unknown

    init(from decoder: Decoder) throws {
        self = ControlSemantic(rawValue: try decoder.singleValueContainer().decode(String.self)) ?? .unknown
    }
}

/// Where the value came from: read from the device, remembered by the driver, or computed.
nonisolated enum ControlOrigin: String, Decodable, Sendable {
    case observed, cached, derived
}

nonisolated enum ControlTarget: Decodable, Sendable, Hashable {
    case node(UInt32)
    case port(UInt32)
    case crosspoint(UInt32)

    private enum CodingKeys: String, CodingKey { case node, port, crosspoint }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        if let id = try c.decodeIfPresent(UInt32.self, forKey: .node) { self = .node(id) }
        else if let id = try c.decodeIfPresent(UInt32.self, forKey: .port) { self = .port(id) }
        else if let id = try c.decodeIfPresent(UInt32.self, forKey: .crosspoint) { self = .crosspoint(id) }
        else {
            throw DecodingError.dataCorrupted(.init(codingPath: decoder.codingPath,
                                                    debugDescription: "A target names a node, port or crosspoint"))
        }
    }

    var isNode: Bool { if case .node = self { true } else { false } }
}

nonisolated struct ControlScalarDomain: Sendable, Equatable {
    /// Nil when the driver could not publish a finite bound.
    var min: Double?
    var max: Double?
    var step: Double?
    /// "dB", "Hz", "%", "normalized" or "generic".
    var unit: String

    /// The finite range a slider can show, or nil.
    var range: ClosedRange<Double>? {
        guard let min, let max, min.isFinite, max.isFinite, min < max else { return nil }
        return min...max
    }

    /// The legal value nearest `value`: clamped to the range and, when the
    /// domain has a step, moved onto the grid `min + k * step` that the driver
    /// checks (it refuses an off-grid value). The highest grid point never
    /// passes `max`.
    func snap(_ value: Double) -> Double {
        guard let range else { return value }
        let clamped = Swift.min(Swift.max(value, range.lowerBound), range.upperBound)
        guard let step, step > 0, step.isFinite else { return clamped }
        let span = range.upperBound - range.lowerBound
        // The small slack keeps a top that is exactly on the grid reachable despite rounding.
        let highest = (span / step + 1e-9).rounded(.down)
        let k = Swift.min(Swift.max(((clamped - range.lowerBound) / step).rounded(), 0), highest)
        return range.lowerBound + k * step
    }
}

nonisolated struct ControlEnumItem: Decodable, Sendable, Equatable, Hashable {
    var value: Int64
    var name: String
}

nonisolated enum ControlDomain: Decodable, Sendable, Equatable {
    case bool
    case action
    case scalar(ControlScalarDomain)
    case enumeration([ControlEnumItem])

    private enum CodingKeys: String, CodingKey { case kind, min, max, step, unit, values }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        switch try c.decode(String.self, forKey: .kind) {
        case "action": self = .action
        case "bool":
            self = .bool
        case "scalar":
            self = .scalar(ControlScalarDomain(min: try c.decodeIfPresent(Double.self, forKey: .min),
                                               max: try c.decodeIfPresent(Double.self, forKey: .max),
                                               step: try c.decodeIfPresent(Double.self, forKey: .step),
                                               unit: try c.decodeIfPresent(String.self, forKey: .unit) ?? "generic"))
        case "enum":
            self = .enumeration(try c.decode([ControlEnumItem].self, forKey: .values))
        case let other:
            throw DecodingError.dataCorruptedError(forKey: .kind, in: c, debugDescription: "Unknown domain kind \(other)")
        }
    }
}

/// A parameter value, and how it travels to the driver: a kind (0 bool,
/// 1 double, 2 int64) and 64 bits (a double as its IEEE-754 bit pattern).
nonisolated enum ControlValue: Sendable, Equatable {
    case bool(Bool)
    case double(Double)
    case int(Int64)
    case action

    enum WireKind: UInt64, Sendable { case bool = 0, double = 1, int = 2, action = 3 }

    var wire: (kind: UInt64, bits: UInt64) {
        switch self {
        case .bool(let value): (WireKind.bool.rawValue, value ? 1 : 0)
        case .double(let value): (WireKind.double.rawValue, value.bitPattern)
        case .int(let value): (WireKind.int.rawValue, UInt64(bitPattern: value))
        case .action: (WireKind.action.rawValue, 0)
        }
    }

    init?(kind: UInt64, bits: UInt64) {
        switch WireKind(rawValue: kind) {
        case .bool: guard bits <= 1 else { return nil }; self = .bool(bits != 0)
        case .double: self = .double(Double(bitPattern: bits))
        case .int: self = .int(Int64(bitPattern: bits))
        case .action: guard bits == 0 else { return nil }; self = .action
        case nil: return nil
        }
    }

    var boolValue: Bool? { if case .bool(let value) = self { value } else { nil } }
    var doubleValue: Double? { if case .double(let value) = self { value } else { nil } }
    var intValue: Int64? { if case .int(let value) = self { value } else { nil } }
}

nonisolated enum ControlAccess: String, Decodable, Sendable { case readWrite, readOnly }

nonisolated struct ControlParameter: Decodable, Sendable, Equatable, Identifiable {
    var id: UInt32
    var name: String
    var semantic: ControlSemantic
    var target: ControlTarget
    var domain: ControlDomain
    /// Nil when the driver holds no value for it.
    var value: ControlValue?
    var origin: ControlOrigin?
    var access: ControlAccess = .readWrite
    var applicable: Bool = true
    var domainOverride: ControlDomain?
    var effectiveDomain: ControlDomain { domainOverride ?? domain }
    var isWritable: Bool { access == .readWrite && applicable }

    private enum CodingKeys: String, CodingKey { case id, name, semantic, target, domain, value, origin, access, applicable, domainOverride }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        id = try c.decode(UInt32.self, forKey: .id)
        name = try c.decode(String.self, forKey: .name)
        semantic = try c.decode(ControlSemantic.self, forKey: .semantic)
        target = try c.decode(ControlTarget.self, forKey: .target)
        domain = try c.decode(ControlDomain.self, forKey: .domain)
        origin = try c.decodeIfPresent(ControlOrigin.self, forKey: .origin)
        access = try c.decodeIfPresent(ControlAccess.self, forKey: .access) ?? .readWrite
        applicable = try c.decodeIfPresent(Bool.self, forKey: .applicable) ?? true
        domainOverride = try c.decodeIfPresent(ControlDomain.self, forKey: .domainOverride)
        if let domainOverride {
            guard case .scalar(let base) = domain, case .scalar(let current) = domainOverride,
                  base.unit == current.unit, let low = current.min, let high = current.max,
                  low.isFinite, high.isFinite, low <= high,
                  current.step.map({ $0.isFinite && $0 > 0 }) ?? true else {
                throw DecodingError.dataCorruptedError(forKey: .domainOverride, in: c,
                                                       debugDescription: "Invalid state range override")
            }
        }
        // The value's JSON type follows the domain: a wrong type is a driver
        // bug and fails the decode rather than showing a guess.
        switch domain {
        case .action: value = nil; origin = nil
        case .bool: value = try c.decodeIfPresent(Bool.self, forKey: .value).map(ControlValue.bool)
        case .scalar: value = try c.decodeIfPresent(Double.self, forKey: .value).map(ControlValue.double)
        case .enumeration: value = try c.decodeIfPresent(Int64.self, forKey: .value).map(ControlValue.int)
        }
    }

    /// The value to send for `requested`: a scalar moved onto its grid; others unchanged.
    /// Nil when `requested` does not fit the domain.
    func legalValue(_ requested: ControlValue) -> ControlValue? {
        switch (effectiveDomain, requested) {
        case (.action, .action): requested
        case (.bool, .bool): requested
        case (.scalar(let scalar), .double(let value)) where value.isFinite: .double(scalar.snap(value))
        case (.enumeration(let items), .int(let value)): items.contains { $0.value == value } ? requested : nil
        default: nil
        }
    }
}

/// Confirmed parameter push on the existing status notification channel.
/// Layout matches ASFWDriverUserClient::NotifyControlParameterChanged.
nonisolated struct ControlParameterChange: Sendable, Equatable {
    static let notificationReason: UInt64 = 0x4354524c // "CTRL"
    var guid: UInt64
    var revision: UInt64
    var sequence: UInt64
    var parameterId: UInt32
    var value: ControlValue
    var origin: ControlOrigin
    var stateInvalidated: Bool = false

    init(guid: UInt64, revision: UInt64, sequence: UInt64, parameterId: UInt32,
         value: ControlValue, origin: ControlOrigin = .observed) {
        self.guid = guid; self.revision = revision; self.sequence = sequence
        self.parameterId = parameterId; self.value = value; self.origin = origin
    }

    init?(scalars: [UInt64]) {
        if scalars.count == 8, scalars[1] == Self.notificationReason,
           scalars[4] == 0, scalars[5] == 4, scalars[6] == 0, scalars[7] == 0 {
            self.init(guid: scalars[2], revision: scalars[3], sequence: scalars[0],
                      parameterId: 0, value: .bool(false))
            stateInvalidated = true
            return
        }
        guard scalars.count == 8, scalars[1] == Self.notificationReason,
              scalars[4] <= UInt32.max, scalars[7] == 0, scalars[5] < 3,
              let value = ControlValue(kind: scalars[5], bits: scalars[6]) else { return nil }
        if case .double(let number) = value, !number.isFinite { return nil }
        if scalars[5] == 0 && scalars[6] > 1 { return nil }
        self.init(guid: scalars[2], revision: scalars[3], sequence: scalars[0],
                  parameterId: UInt32(scalars[4]), value: value)
    }
}
