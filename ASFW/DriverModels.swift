//
//  DriverModels.swift
//  ASFW
//
//  Created by ASFireWire Project on 07.10.2025.
//

import Foundation

// MARK: - Wire Format Structures (C-compatible)

/// Wire format for async descriptor metadata
struct ControllerStatusAsyncDescriptorWire {
    var descriptorVirt: UInt64
    var descriptorIOVA: UInt64
    var descriptorCount: UInt32
    var descriptorStride: UInt32
    var commandPtr: UInt32
    var reserved: UInt32
}

/// Wire format for async buffer pools
struct ControllerStatusAsyncBuffersWire {
    var bufferVirt: UInt64
    var bufferIOVA: UInt64
    var bufferCount: UInt32
    var bufferSize: UInt32
}

/// Wire format for async subsystem snapshot
struct ControllerStatusAsyncWire {
    var atRequest: ControllerStatusAsyncDescriptorWire
    var atResponse: ControllerStatusAsyncDescriptorWire
    var arRequest: ControllerStatusAsyncDescriptorWire
    var arResponse: ControllerStatusAsyncDescriptorWire
    var arRequestBuffers: ControllerStatusAsyncBuffersWire
    var arResponseBuffers: ControllerStatusAsyncBuffersWire
    var dmaSlabVirt: UInt64
    var dmaSlabIOVA: UInt64
    var dmaSlabSize: UInt32
    var reserved: UInt32
}

/// Wire format for controller status (matches driver's ControllerStatusWire)
struct ControllerStatusWire {
    var version: UInt32
    var flags: UInt32
    var stateName: (UInt8, UInt8, UInt8, UInt8, UInt8, UInt8, UInt8, UInt8,
                   UInt8, UInt8, UInt8, UInt8, UInt8, UInt8, UInt8, UInt8,
                   UInt8, UInt8, UInt8, UInt8, UInt8, UInt8, UInt8, UInt8,
                   UInt8, UInt8, UInt8, UInt8, UInt8, UInt8, UInt8, UInt8) // 32 bytes
    var generation: UInt32
    var nodeCount: UInt32
    var localNodeID: UInt32
    var rootNodeID: UInt32
    var irmNodeID: UInt32
    var busResetCount: UInt64
    var lastBusResetTime: UInt64
    var uptimeNanoseconds: UInt64
    var async: ControllerStatusAsyncWire
}

/// Wire format for bus reset packet snapshot
struct BusResetPacketWire {
    var captureTimestamp: UInt64
    var generation: UInt32
    var eventCode: UInt8
    var tCode: UInt8
    var cycleTime: UInt16
    var rawQuadlets: (UInt32, UInt32, UInt32, UInt32)
    var wireQuadlets: (UInt32, UInt32, UInt32, UInt32)
    var contextInfo: (UInt8, UInt8, UInt8, UInt8, UInt8, UInt8, UInt8, UInt8,
                     UInt8, UInt8, UInt8, UInt8, UInt8, UInt8, UInt8, UInt8,
                     UInt8, UInt8, UInt8, UInt8, UInt8, UInt8, UInt8, UInt8,
                     UInt8, UInt8, UInt8, UInt8, UInt8, UInt8, UInt8, UInt8,
                     UInt8, UInt8, UInt8, UInt8, UInt8, UInt8, UInt8, UInt8,
                     UInt8, UInt8, UInt8, UInt8, UInt8, UInt8, UInt8, UInt8,
                     UInt8, UInt8, UInt8, UInt8, UInt8, UInt8, UInt8, UInt8,
                     UInt8, UInt8, UInt8, UInt8, UInt8, UInt8, UInt8, UInt8) // 64 bytes
}

/// Wire format for Self-ID metrics (matches C++ SelfIDMetricsWire)
struct SelfIDMetricsWire {
    var generation: UInt32
    var captureTimestamp: UInt64
    var quadletCount: UInt32
    var sequenceCount: UInt32
    var valid: UInt8
    var timedOut: UInt8
    var crcError: UInt8
    var _padding: UInt8
    var errorReason: (UInt8, UInt8, UInt8, UInt8, UInt8, UInt8, UInt8, UInt8,
                     UInt8, UInt8, UInt8, UInt8, UInt8, UInt8, UInt8, UInt8,
                     UInt8, UInt8, UInt8, UInt8, UInt8, UInt8, UInt8, UInt8,
                     UInt8, UInt8, UInt8, UInt8, UInt8, UInt8, UInt8, UInt8,
                     UInt8, UInt8, UInt8, UInt8, UInt8, UInt8, UInt8, UInt8,
                     UInt8, UInt8, UInt8, UInt8, UInt8, UInt8, UInt8, UInt8,
                     UInt8, UInt8, UInt8, UInt8, UInt8, UInt8, UInt8, UInt8,
                     UInt8, UInt8, UInt8, UInt8, UInt8, UInt8, UInt8, UInt8) // 64 bytes
}

struct SelfIDSequenceWire {
    var startIndex: UInt32 = 0
    var quadletCount: UInt32 = 0
}

// Topology no longer has a hand-mirrored Swift wire struct: it is served via the
// diagnostics ABI (ASFWDiagTopology / ASFWDiagNode), imported directly from the C
// header through the bridging header, so layout is guaranteed to match the driver.
// See TopologySnapshot.from(diag:) / TopologyNode(diag:) below.

// MARK: - Swift Data Models

/// Controller state information
struct ControllerStatus {
    struct AsyncContextStatus {
        let descriptorVirt: UInt64
        let descriptorIOVA: UInt64
        let descriptorCount: UInt32
        let descriptorStride: UInt32
        let commandPtr: UInt32
    }

    struct AsyncBufferStatus {
        let bufferVirt: UInt64
        let bufferIOVA: UInt64
        let bufferCount: UInt32
        let bufferSize: UInt32
    }

    struct AsyncStatus {
        let atRequest: AsyncContextStatus
        let atResponse: AsyncContextStatus
        let arRequest: AsyncContextStatus
        let arResponse: AsyncContextStatus
        let arRequestBuffers: AsyncBufferStatus
        let arResponseBuffers: AsyncBufferStatus
        let dmaSlabVirt: UInt64
        let dmaSlabIOVA: UInt64
        let dmaSlabSize: UInt32
    }

    let stateName: String
    let generation: UInt32
    let busResetCount: UInt64
    let lastBusResetTime: UInt64
    let uptimeNanoseconds: UInt64
    let nodeCount: UInt32
    let localNodeID: UInt8?
    let rootNodeID: UInt8?
    let irmNodeID: UInt8?
    let isIRM: Bool
    let isCycleMaster: Bool
    let async: AsyncStatus

    init(wire: ControllerStatusWire) {
        // Convert tuple to array for String conversion
        let nameBytes: [UInt8] = withUnsafeBytes(of: wire.stateName) { ptr in
            Array(ptr.bindMemory(to: UInt8.self))
        }

        // Find null terminator
        let count = nameBytes.firstIndex(of: 0) ?? nameBytes.count
        self.stateName = String(bytes: nameBytes[..<count], encoding: .utf8) ?? "Unknown"

        self.generation = wire.generation
        self.busResetCount = wire.busResetCount
        self.lastBusResetTime = wire.lastBusResetTime
        self.uptimeNanoseconds = wire.uptimeNanoseconds
        self.nodeCount = wire.nodeCount
        self.localNodeID = wire.localNodeID == 0xFFFF_FFFF ? nil : UInt8(truncatingIfNeeded: wire.localNodeID)
        self.rootNodeID = wire.rootNodeID == 0xFFFF_FFFF ? nil : UInt8(truncatingIfNeeded: wire.rootNodeID)
        self.irmNodeID = wire.irmNodeID == 0xFFFF_FFFF ? nil : UInt8(truncatingIfNeeded: wire.irmNodeID)
        self.isIRM = (wire.flags & 0x1) != 0
        self.isCycleMaster = (wire.flags & 0x2) != 0

        func makeContext(from wire: ControllerStatusAsyncDescriptorWire) -> AsyncContextStatus {
            AsyncContextStatus(
                descriptorVirt: wire.descriptorVirt,
                descriptorIOVA: wire.descriptorIOVA,
                descriptorCount: wire.descriptorCount,
                descriptorStride: wire.descriptorStride,
                commandPtr: wire.commandPtr
            )
        }

        func makeBuffers(from wire: ControllerStatusAsyncBuffersWire) -> AsyncBufferStatus {
            AsyncBufferStatus(
                bufferVirt: wire.bufferVirt,
                bufferIOVA: wire.bufferIOVA,
                bufferCount: wire.bufferCount,
                bufferSize: wire.bufferSize
            )
        }

        self.async = AsyncStatus(
            atRequest: makeContext(from: wire.async.atRequest),
            atResponse: makeContext(from: wire.async.atResponse),
            arRequest: makeContext(from: wire.async.arRequest),
            arResponse: makeContext(from: wire.async.arResponse),
            arRequestBuffers: makeBuffers(from: wire.async.arRequestBuffers),
            arResponseBuffers: makeBuffers(from: wire.async.arResponseBuffers),
            dmaSlabVirt: wire.async.dmaSlabVirt,
            dmaSlabIOVA: wire.async.dmaSlabIOVA,
            dmaSlabSize: wire.async.dmaSlabSize
        )
    }

    var formattedLocalNodeID: String {
        guard let value = localNodeID else { return "--" }
        return String(format: "0x%02X", value)
    }

    var formattedRootNodeID: String {
        guard let value = rootNodeID else { return "--" }
        return String(format: "0x%02X", value)
    }

    var formattedIRMNodeID: String {
        guard let value = irmNodeID else { return "--" }
        return String(format: "0x%02X", value)
    }
}

/// Bus reset packet snapshot
struct BusResetPacketSnapshot: Identifiable, Hashable {
    let id = UUID()
    let captureTimestamp: UInt64
    let generation: UInt32
    let eventCode: UInt8
    let tCode: UInt8
    let cycleTime: UInt16
    let rawQuadlets: [UInt32]
    let wireQuadlets: [UInt32]
    let contextInfo: String
    
    init(wire: BusResetPacketWire) {
        self.captureTimestamp = wire.captureTimestamp
        self.generation = wire.generation
        self.eventCode = wire.eventCode
        self.tCode = wire.tCode
        self.cycleTime = wire.cycleTime
        self.rawQuadlets = [wire.rawQuadlets.0, wire.rawQuadlets.1, 
                           wire.rawQuadlets.2, wire.rawQuadlets.3]
        self.wireQuadlets = [wire.wireQuadlets.0, wire.wireQuadlets.1, 
                            wire.wireQuadlets.2, wire.wireQuadlets.3]
        
        // Convert context info tuple to string
        let contextBytes: [UInt8] = withUnsafeBytes(of: wire.contextInfo) { ptr in
            Array(ptr.bindMemory(to: UInt8.self))
        }
        let count = contextBytes.firstIndex(of: 0) ?? contextBytes.count
        self.contextInfo = String(bytes: contextBytes[..<count], encoding: .utf8) ?? ""
    }
    
    // Hashable conformance
    func hash(into hasher: inout Hasher) {
        hasher.combine(id)
    }
    
    static func == (lhs: BusResetPacketSnapshot, rhs: BusResetPacketSnapshot) -> Bool {
        lhs.id == rhs.id
    }
}

// MARK: - Port State Enum

/// Port state enum matching C++ TopologyTypes.hpp PortState
enum PortState: UInt8 {
    case notPresent = 0   // No port present
    case notActive = 1    // Port present but not active
    case parent = 2       // Parent port (connection to upstream node)
    case child = 3        // Child port (connection to downstream node)

    var icon: String {
        switch self {
        case .notPresent: return "◽️"
        case .notActive: return "⚫️"
        case .parent: return "🔼"
        case .child: return "🔽"
        }
    }

    var description: String {
        switch self {
        case .notPresent: return "Not Present"
        case .notActive: return "Not Active"
        case .parent: return "Parent"
        case .child: return "Child"
        }
    }
}

/// Physical adjacency for a single PHY port (parallel to a node's portStates).
/// Decoded from ASFWDiagNode.links[]: 0xFFFFFFFF means the port is not connected,
/// otherwise bits [15:8] = remote node id, bits [7:0] = remote port.
struct PortLink: Equatable {
    let connected: Bool
    let remoteNodeId: UInt8
    let remotePort: UInt8

    static let unconnected = PortLink(connected: false, remoteNodeId: 0xFF, remotePort: 0xFF)

    init(connected: Bool, remoteNodeId: UInt8, remotePort: UInt8) {
        self.connected = connected
        self.remoteNodeId = remoteNodeId
        self.remotePort = remotePort
    }

    init(raw: UInt32) {
        if raw == 0xFFFF_FFFF {
            self = .unconnected
        } else {
            self.init(connected: true,
                      remoteNodeId: UInt8((raw >> 8) & 0xFF),
                      remotePort: UInt8(raw & 0xFF))
        }
    }
}

/// Maps the ASFWDiagSpeed enum (S100..S3200) to a Mbps figure for display.
func mbpsFromDiagSpeed(_ speed: UInt32) -> UInt32 {
    switch speed {
    case ASFWDiagSpeedS100.rawValue: return 100
    case ASFWDiagSpeedS200.rawValue: return 200
    case ASFWDiagSpeedS400.rawValue: return 400
    case ASFWDiagSpeedS800.rawValue: return 800
    case ASFWDiagSpeedS1600.rawValue: return 1600
    case ASFWDiagSpeedS3200.rawValue: return 3200
    default: return 0
    }
}

// MARK: - Self-ID Models

/// Decoded Self-ID sequence for a single node
struct SelfIDPacket: Identifiable {
    let id = UUID()
    let physicalID: UInt8
    let quadlets: [UInt32]
    let linkActive: Bool
    let gapCount: UInt8
    let speedMbps: UInt16
    let powerClass: UInt8
    let portStates: [PortState]
    let isContender: Bool
    let initiatedReset: Bool
    
    init(quadlets: [UInt32]) {
        self.quadlets = quadlets
        guard !quadlets.isEmpty else {
            self.physicalID = 0xFF
            self.linkActive = false
            self.gapCount = 0
            self.speedMbps = 0
            self.powerClass = 0
            self.portStates = []
            self.isContender = false
            self.initiatedReset = false
            return
        }
        
        let q0 = quadlets[0]
        self.physicalID = UInt8((q0 >> 24) & 0x3F)
        self.linkActive = ((q0 >> 22) & 1) != 0
        self.gapCount = UInt8((q0 >> 16) & 0x3F)
        
        let spd = (q0 >> 14) & 0x3
        self.speedMbps = [100, 200, 400, 800][Int(spd)]
        
        self.powerClass = UInt8((q0 >> 8) & 0x7)
        self.isContender = ((q0 >> 11) & 1) != 0
        self.initiatedReset = ((q0 >> 1) & 1) != 0
        
        // Decode port states from quadlet 0 and continuation packets
        var ports: [PortState] = []

        // First 3 ports from quadlet 0
        for i in 0..<3 {
            let shift = 6 - (i * 2)
            let state = (q0 >> shift) & 0x3
            ports.append(PortState(rawValue: UInt8(state)) ?? .notPresent)
        }

        // Additional ports from continuation quadlets
        for qi in 1..<quadlets.count {
            let qn = quadlets[qi]
            for i in 0..<8 {
                let shift = 16 - (i * 2)
                let state = (qn >> shift) & 0x3
                ports.append(PortState(rawValue: UInt8(state)) ?? .notPresent)
            }
        }

        self.portStates = ports
    }
}

/// Complete Self-ID capture data
struct SelfIDCapture: Identifiable {
    let id = UUID()
    let generation: UInt32
    let captureTimestamp: UInt64
    let valid: Bool
    let timedOut: Bool
    let crcError: Bool
    let errorReason: String?
    let rawQuadlets: [UInt32]
    let packets: [SelfIDPacket]
    
    /// Decode from wire format data
    static func decode(from data: Data) -> SelfIDCapture? {
        print("🔍 SelfIDCapture.decode: got \(data.count) bytes")
        print("🔍 First 64 bytes (hex): \(data.prefix(64).map { String(format: "%02x", $0) }.joined(separator: " "))")
        
        let headerSize = MemoryLayout<SelfIDMetricsWire>.size
        guard data.count >= headerSize else { return nil }
        
        return data.withUnsafeBytes { raw -> SelfIDCapture? in
            guard let base = raw.baseAddress else { return nil }
            func read<T: FixedWidthInteger>(_ offset: Int, as type: T.Type) -> T {
                precondition(offset + MemoryLayout<T>.size <= raw.count)
                var value: T = 0
                withUnsafeMutableBytes(of: &value) { dest in
                    let slice = UnsafeRawBufferPointer(start: base.advanced(by: offset), count: MemoryLayout<T>.size)
                    dest.copyBytes(from: slice)
                }
                return T(littleEndian: value)
            }
            func readUInt8(_ offset: Int) -> UInt8 {
                precondition(offset < raw.count)
                return raw[offset]
            }
            
            let generation: UInt32 = read(0, as: UInt32.self)
            let captureTimestamp: UInt64 = read(4, as: UInt64.self)
            let quadletCount: UInt32 = read(12, as: UInt32.self)
            let sequenceCount: UInt32 = read(16, as: UInt32.self)
            let valid = readUInt8(20)
            let timedOut = readUInt8(21)
            let crcError = readUInt8(22)
            // Skip padding at 23
            
            // Parse error reason
            var errorBytes = [UInt8](repeating: 0, count: 64)
            errorBytes.withUnsafeMutableBytes { dest in
                let slice = UnsafeRawBufferPointer(start: base.advanced(by: 24), count: 64)
                dest.copyBytes(from: slice)
            }
            let errorCount = errorBytes.firstIndex(of: 0) ?? errorBytes.count
            let errorReason = errorCount > 0 ? String(bytes: errorBytes[..<errorCount], encoding: .utf8) : nil
            
            // Read quadlets
            var offset = headerSize
            var quadlets: [UInt32] = []
            for _ in 0..<quadletCount {
                guard offset + MemoryLayout<UInt32>.size <= raw.count else { break }
                let quadlet: UInt32 = read(offset, as: UInt32.self)
                quadlets.append(quadlet)
                offset += MemoryLayout<UInt32>.size
            }
            
            // Read sequences
            var sequences: [(start: Int, count: Int)] = []
            for _ in 0..<sequenceCount {
                guard offset + MemoryLayout<SelfIDSequenceWire>.size <= raw.count else { break }
                var seqWire = SelfIDSequenceWire()
                withUnsafeMutableBytes(of: &seqWire) { dest in
                    let slice = UnsafeRawBufferPointer(start: base.advanced(by: offset), count: MemoryLayout<SelfIDSequenceWire>.size)
                    dest.copyBytes(from: slice)
                }
                sequences.append((Int(seqWire.startIndex), Int(seqWire.quadletCount)))
                offset += MemoryLayout<SelfIDSequenceWire>.size
            }
            
            // Decode packets from sequences
            var packets: [SelfIDPacket] = []
            for (start, count) in sequences {
                let end = min(start + count, quadlets.count)
                guard start < quadlets.count, end > start else { continue }
                let packet = SelfIDPacket(quadlets: Array(quadlets[start..<end]))
                packets.append(packet)
            }
            
            print("🔍 Parsed SelfID: quadlets=\(quadlets.count) sequences=\(sequences.count) valid=\(valid)")
            
            return SelfIDCapture(
                generation: generation,
                captureTimestamp: captureTimestamp,
                valid: valid != 0,
                timedOut: timedOut != 0,
                crcError: crcError != 0,
                errorReason: errorReason,
                rawQuadlets: quadlets,
                packets: packets
            )
        }
    }
}

// MARK: - Topology Models

/// Single node in the topology
struct TopologyNode: Identifiable {
    let id: UInt8  // nodeId serves as ID
    let nodeId: UInt8
    let portCount: UInt8
    let gapCount: UInt8
    let powerClass: UInt8
    let maxSpeedMbps: UInt32
    let isIRMCandidate: Bool
    let linkActive: Bool
    let initiatedReset: Bool
    let isRoot: Bool
    let parentPort: UInt8?
    let portStates: [PortState]
    /// Physical adjacency parallel to portStates (index = port number).
    let links: [PortLink]

    var speedDescription: String {
        "\(maxSpeedMbps) Mbps"
    }

    var powerDescription: String {
        ["No power", "Self-powered (15W)", "Bus-powered (1.5W)", "Bus-powered (3W)",
         "Bus-powered (6W)", "Self-powered (10W)", "Reserved", "Reserved"][Int(powerClass)]
    }

    /// Build from a diagnostics-ABI node. ports[]/links[] are fixed C arrays
    /// imported as tuples; only the first portCount entries are meaningful.
    init(diag n: ASFWDiagNode) {
        let portCount = Int(min(n.portCount, UInt32(ASFW_DIAG_MAX_PORTS)))
        let rawPorts: [UInt32] = withUnsafeBytes(of: n.ports) { Array($0.bindMemory(to: UInt32.self)) }
        let rawLinks: [UInt32] = withUnsafeBytes(of: n.links) { Array($0.bindMemory(to: UInt32.self)) }

        self.id = UInt8(truncatingIfNeeded: n.nodeId)
        self.nodeId = UInt8(truncatingIfNeeded: n.nodeId)
        self.portCount = UInt8(truncatingIfNeeded: n.portCount)
        self.gapCount = UInt8(truncatingIfNeeded: n.gapCount)
        self.powerClass = UInt8(truncatingIfNeeded: n.powerClass & 0x7)
        self.maxSpeedMbps = mbpsFromDiagSpeed(n.speed)
        self.isIRMCandidate = n.contender != 0
        self.linkActive = n.linkActive != 0
        self.initiatedReset = n.initiatedReset != 0
        self.isRoot = n.isRoot != 0
        self.parentPort = n.parentPort == 0xFFFF_FFFF ? nil : UInt8(truncatingIfNeeded: n.parentPort)
        self.portStates = (0..<portCount).map { PortState(rawValue: UInt8(truncatingIfNeeded: rawPorts[$0])) ?? .notPresent }
        self.links = (0..<portCount).map { PortLink(raw: rawLinks[$0]) }
    }

    init(id: UInt8, nodeId: UInt8, portCount: UInt8, gapCount: UInt8, powerClass: UInt8,
         maxSpeedMbps: UInt32, isIRMCandidate: Bool, linkActive: Bool, initiatedReset: Bool,
         isRoot: Bool, parentPort: UInt8?, portStates: [PortState], links: [PortLink]) {
        self.id = id
        self.nodeId = nodeId
        self.portCount = portCount
        self.gapCount = gapCount
        self.powerClass = powerClass
        self.maxSpeedMbps = maxSpeedMbps
        self.isIRMCandidate = isIRMCandidate
        self.linkActive = linkActive
        self.initiatedReset = initiatedReset
        self.isRoot = isRoot
        self.parentPort = parentPort
        self.portStates = portStates
        self.links = links
    }
}

/// Complete topology snapshot
struct TopologySnapshot: Identifiable {
    let id = UUID()
    let generation: UInt32
    let capturedAt: UInt64
    let nodeCount: UInt8
    let rootNodeId: UInt8?
    let irmNodeId: UInt8?
    let localNodeId: UInt8?
    let gapCount: UInt8
    let busBase16: UInt16         // Bus base (bus << 6), ready to OR with node ID
    let nodes: [TopologyNode]
    let warnings: [String]
    
    init(generation: UInt32, capturedAt: UInt64, nodeCount: UInt8, rootNodeId: UInt8?,
         irmNodeId: UInt8?, localNodeId: UInt8?, gapCount: UInt8, busBase16: UInt16,
         nodes: [TopologyNode], warnings: [String]) {
        self.generation = generation
        self.capturedAt = capturedAt
        self.nodeCount = nodeCount
        self.rootNodeId = rootNodeId
        self.irmNodeId = irmNodeId
        self.localNodeId = localNodeId
        self.gapCount = gapCount
        self.busBase16 = busBase16
        self.nodes = nodes
        self.warnings = warnings
    }

    /// Build from the diagnostics-ABI topology struct (ASFWDiagTopology), which is
    /// imported directly from the shared C header — no hand-mirrored layout.
    /// Returns nil when the driver reports the topology is not valid.
    static func from(diag t: ASFWDiagTopology) -> TopologySnapshot? {
        guard t.valid != 0 else { return nil }

        // Valid bus node IDs are 0..62; 0x3F (63) and 0xFF are "no node" sentinels.
        func nodeOpt(_ value: UInt32) -> UInt8? { value >= 0x3F ? nil : UInt8(truncatingIfNeeded: value) }

        // The driver clamps nodeCount to what fit in the inline wire buffer, so the
        // remaining nodes[] entries are zero-filled and must not be read.
        let count = Int(min(t.nodeCount, UInt32(ASFW_DIAG_MAX_NODES)))
        let diagNodes: [ASFWDiagNode] = withUnsafeBytes(of: t.nodes) { buffer in
            Array(buffer.bindMemory(to: ASFWDiagNode.self).prefix(count))
        }
        let nodes = diagNodes.map { TopologyNode(diag: $0) }

        var warnings: [String] = []
        if t.enumeratorError != 0 {
            warnings.append("Topology enumerator reported an error (Self-ID not valid).")
        }

        return TopologySnapshot(
            generation: t.header.generation,
            capturedAt: t.header.timestampNs,
            nodeCount: UInt8(truncatingIfNeeded: t.nodeCount),
            rootNodeId: nodeOpt(t.rootNode),
            irmNodeId: nodeOpt(t.irmNode),
            localNodeId: nodeOpt(t.localNode),
            gapCount: UInt8(truncatingIfNeeded: t.gapCount),
            busBase16: UInt16(truncatingIfNeeded: t.busBase16),
            nodes: nodes,
            warnings: warnings
        )
    }
}
