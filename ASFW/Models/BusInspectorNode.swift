import Foundation

/// Joins discovery identity to a current-generation topology node.
struct BusInspectorNode: Identifiable {
    let node: TopologyNode
    let device: ASFWDriverConnector.FWDeviceInfo?
    let generation: UInt32
    let isLocal: Bool

    // GUIDs survive node renumbering. Unknown nodes are scoped to their generation.
    var id: String {
        if let device { return "guid:\(device.guid)" }
        return "generation:\(generation):node:\(node.nodeId)"
    }

    var title: String {
        if let device {
            let name = "\(device.vendorName) \(device.modelName)"
                .trimmingCharacters(in: .whitespacesAndNewlines)
            if !name.isEmpty { return name }
            return String(format: "Device 0x%016llX", device.guid)
        }
        return isLocal ? "Local controller" : "Node \(node.nodeId)"
    }

    func isAVCDevice(registeredGUIDs: [UInt64]) -> Bool {
        guard let device else { return false }
        return device.deviceKind == 1 ||
            device.units.contains { $0.specId == 0x00A02D && $0.swVersion == 0x010001 } ||
            registeredGUIDs.contains(device.guid)
    }

    var isDICEDevice: Bool {
        guard let device else { return false }
        // Match the ROM signature used by DeviceProfiles/Audio/DiceIdentity.hpp.
        // Discovery exposes specifier/version but not the unit model ID, so
        // navigation checks the GUID vendor/category prefix without probing registers.
        return device.units.contains { unit in
            guard unit.state != .terminated, unit.swVersion == 0x000001 else { return false }
            let category: UInt32
            switch unit.specId {
            case 0x001C6A: category = 0x00 // Weiss
            case 0x000FF2: category = 0x10 // LOUD
            case 0x000FD7: category = 0x20 // Harman
            default: category = 0x04
            }
            return unit.specId != 0 && UInt32(device.guid >> 32) == (unit.specId << 8 | category)
        }
    }

    static func inventory(topology: TopologySnapshot?,
                          devices: [ASFWDriverConnector.FWDeviceInfo]) -> [Self] {
        guard let topology else { return [] }
        return topology.nodes.sorted { $0.nodeId < $1.nodeId }.map { node in
            Self(node: node,
                 device: devices.first {
                     $0.nodeId == node.nodeId && $0.generation == topology.generation && $0.state != .terminated
                 },
                 generation: topology.generation,
                 isLocal: node.nodeId == topology.localNodeId)
        }
    }
}
