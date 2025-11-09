//
//  RomModels.swift
//  ASFW
//
//  Core data structures for Config ROM parsing
//  Integrated from RomExplorer tool
//

import Foundation

// MARK: - Core ROM Data Structures

public struct BusInfo: Sendable, Codable {
    public var irmc: UInt32
    public var cmc: UInt32
    public var isc: UInt32
    public var bmc: UInt32
    public var cycClkAcc: UInt32
    public var maxRec: UInt32
    public var pmc: UInt32
    public var gen: UInt32
    public var linkSpd: UInt32
    public var adj: UInt32
    public var nodeVendorID: UInt32
    public var chipID: UInt64
}

public enum EntryType: UInt8, Codable, Sendable {
    case immediate = 0x00
    case csrOffset = 0x01
    case leaf = 0x02
    case directory = 0x03
}

public struct DirectoryEntry: Codable, Sendable, Identifiable {
    public var id: String { "\(keyId)-\(type.rawValue)" }
    public var keyId: UInt8
    public var type: EntryType
    public var value: RomValue
    public var keyName: String { KeyType.name(for: keyId) }
}

public enum RomValue: Codable, Sendable {
    case immediate(UInt32)
    case csrOffset(UInt64)
    case leafPlaceholder(offset: Int)
    case leafDescriptorText(String, Data)
    case leafEUI64(UInt64)
    case leafData(Data)
    case directory([DirectoryEntry])
}

public struct RomTree: Codable, Sendable {
    public var busInfoRaw: Data
    public var busInfo: BusInfo
    public var rootDirectory: [DirectoryEntry]
}

public enum RomError: Error, CustomStringConvertible {
    case invalidData(String)
    case unsupported(String)

    public var description: String {
        switch self {
        case .invalidData(let s): return "Invalid data: \(s)"
        case .unsupported(let s): return "Unsupported: \(s)"
        }
    }
}

public enum KeyType: UInt8, Codable, Sendable {
    case descriptor = 0x01
    case busDependentInfo = 0x02
    case vendor = 0x03
    case hardwareVersion = 0x04
    case module = 0x07
    case nodeCapabilities = 0x0c
    case eui64 = 0x0d
    case unit = 0x11
    case specifierId = 0x12
    case version = 0x13
    case dependentInfo = 0x14
    case unitLocation = 0x15
    case model = 0x17
    case instance = 0x18
    case keyword = 0x19
    case feature = 0x1a
    case modifiableDescriptor = 0x1f
    case directoryId = 0x20

    public static func name(for id: UInt8) -> String {
        switch KeyType(rawValue: id) {
        case .descriptor: return "DESCRIPTOR"
        case .busDependentInfo: return "BUS_DEPENDENT_INFO"
        case .vendor: return "VENDOR"
        case .hardwareVersion: return "HARDWARE_VERSION"
        case .module: return "MODULE"
        case .nodeCapabilities: return "NODE_CAPABILITIES"
        case .eui64: return "EUI_64"
        case .unit: return "UNIT"
        case .specifierId: return "SPECIFIER_ID"
        case .version: return "VERSION"
        case .dependentInfo: return "DEPENDENT_INFO"
        case .unitLocation: return "UNIT_LOCATION"
        case .model: return "MODEL"
        case .instance: return "INSTANCE"
        case .keyword: return "KEYWORD"
        case .feature: return "FEATURE"
        case .modifiableDescriptor: return "MODIFIABLE_DESCRIPTOR"
        case .directoryId: return "DIRECTORY_ID"
        case .none:
            return String(format: "Key 0x%X", id)
        }
    }
}

// MARK: - ROM Summary Structures

public struct UnitInfo: Sendable, Codable {
    public var specifierId: UInt32?
    public var version: UInt32?
    public var modelId: UInt32?
    public var modelName: String?
}

public struct RomSummary: Sendable, Codable {
    public var vendorId: UInt32?
    public var vendorName: String?
    public var modelId: UInt32?
    public var modelName: String?
    public var units: [UnitInfo]
    public var modalias: String? // ieee1394:ven..mo..sp..ver..
}

// MARK: - ROM Directory API

public enum RomEntryMaskType {
    case any
    case immediate
    case csrOffset
    case leaf
    case directory
}

public struct RomDirectoryView {
    public let romRaw: Data
    public let entries: [DirectoryEntry]

    public init(rom: RomTree, entries: [DirectoryEntry]? = nil) {
        self.romRaw = rom.busInfoRaw
        self.entries = entries ?? rom.rootDirectory
    }

    public init(romRaw: Data, entries: [DirectoryEntry]) {
        self.romRaw = romRaw
        self.entries = entries
    }

    // Iterator-style helpers
    public func subdirectories(of key: KeyType) -> [[DirectoryEntry]] {
        var out: [[DirectoryEntry]] = []
        for i in entries.indices {
            let e = entries[i]
            if KeyType(rawValue: e.keyId) == key, case .directory(let d) = e.value { out.append(d) }
        }
        return out
    }

    public func units() -> [[DirectoryEntry]] { subdirectories(of: .unit) }
    public func features() -> [[DirectoryEntry]] { subdirectories(of: .feature) }
    public func instances() -> [[DirectoryEntry]] { subdirectories(of: .instance) }

    // Simple key finder
    public func findIndex(key: KeyType, type: RomEntryMaskType = .any, startAt: Int = 0) -> Int? {
        guard startAt >= 0 else { return nil }
        for i in startAt..<entries.count {
            let e = entries[i]
            if KeyType(rawValue: e.keyId) == key, matchesType(e.type, type) {
                return i
            }
        }
        return nil
    }

    private func matchesType(_ t: EntryType, _ mask: RomEntryMaskType) -> Bool {
        switch mask {
        case .any: return true
        case .immediate: return t == .immediate
        case .csrOffset: return t == .csrOffset
        case .leaf: return t == .leaf
        case .directory: return t == .directory
        }
    }

    public func getImmediate(at index: Int) -> UInt32? {
        guard entries.indices.contains(index) else { return nil }
        if case .immediate(let v) = entries[index].value { return v }
        return nil
    }

    public func getOffset(at index: Int) -> UInt64? {
        guard entries.indices.contains(index) else { return nil }
        if case .csrOffset(let v) = entries[index].value { return v }
        return nil
    }

    public func getDirectory(at index: Int) -> [DirectoryEntry]? {
        guard entries.indices.contains(index) else { return nil }
        if case .directory(let d) = entries[index].value { return d }
        return nil
    }

    public func getDescriptorTextAdjacent(to index: Int) -> String? {
        let next = index + 1
        guard entries.indices.contains(next) else { return nil }
        let e = entries[next]
        if KeyType(rawValue: e.keyId) == .descriptor, case .leafDescriptorText(let s, _) = e.value { return s }
        return nil
    }
}
