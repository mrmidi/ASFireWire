//
//  RomModels.swift
//  ASFW
//
//  Core data structures for Config ROM parsing
//  Integrated from RomExplorer tool
//

import Foundation

// MARK: - Core ROM Data Structures

public enum RomDiagnosticSeverity: String, Codable, Sendable {
    case info
    case warning
}

public struct RomDiagnostic: Sendable, Codable, Identifiable {
    public var id: UUID = UUID()
    public var severity: RomDiagnosticSeverity
    public var message: String

    public init(severity: RomDiagnosticSeverity, message: String) {
        self.severity = severity
        self.message = message
    }
}

public struct ConfigROMHeader: Sendable, Codable {
    public var busInfoLength: UInt8
    public var crcLength: UInt8
    public var crc: UInt16
    public var rawQuadlet: UInt32
}

public struct BusOptions: Sendable, Codable {
    public var raw: UInt32
    public var irmc: Bool
    public var cmc: Bool
    public var isc: Bool
    public var bmc: Bool
    public var pmc: Bool
    public var cycClkAcc: UInt8
    public var maxRec: UInt8
    public var maxRom: UInt8
    public var generation: UInt8
    public var linkSpd: UInt8
}

public struct BusInfo: Sendable, Codable {
    public var header: ConfigROMHeader
    public var busName: UInt32
    public var busOptions: BusOptions
    public var guidHigh: UInt32
    public var guidLow: UInt32

    public init(header: ConfigROMHeader, busName: UInt32, busOptions: BusOptions, guidHigh: UInt32, guidLow: UInt32) {
        self.header = header
        self.busName = busName
        self.busOptions = busOptions
        self.guidHigh = guidHigh
        self.guidLow = guidLow
    }

    // Legacy convenience accessors kept for older call sites / summaries.
    public var irmc: UInt32 { busOptions.irmc ? 1 : 0 }
    public var cmc: UInt32 { busOptions.cmc ? 1 : 0 }
    public var isc: UInt32 { busOptions.isc ? 1 : 0 }
    public var bmc: UInt32 { busOptions.bmc ? 1 : 0 }
    public var pmc: UInt32 { busOptions.pmc ? 1 : 0 }
    public var cycClkAcc: UInt32 { UInt32(busOptions.cycClkAcc) }
    public var maxRec: UInt32 { UInt32(busOptions.maxRec) }
    public var gen: UInt32 { UInt32(busOptions.generation) }
    public var linkSpd: UInt32 { UInt32(busOptions.linkSpd) }
    public var maxRom: UInt32 { UInt32(busOptions.maxRom) }
    public var adj: UInt32 { 0 } // no longer decoded from this field; kept for compatibility

    public var guid: UInt64 { (UInt64(guidHigh) << 32) | UInt64(guidLow) }
    public var nodeVendorID: UInt32 { (guidHigh >> 8) & 0x00ff_ffff }
    public var chipID: UInt64 { (UInt64(guidHigh & 0x0000_00ff) << 32) | UInt64(guidLow) }
    public var busNameString: String {
        let bytes: [UInt8] = [
            UInt8((busName >> 24) & 0xff),
            UInt8((busName >> 16) & 0xff),
            UInt8((busName >> 8) & 0xff),
            UInt8(busName & 0xff)
        ]
        return String(bytes: bytes, encoding: .ascii) ?? String(format: "0x%08X", busName)
    }
}

public enum EntryType: UInt8, Codable, Sendable {
    case immediate = 0x00
    case csrOffset = 0x01
    case leaf = 0x02
    case directory = 0x03
}

public struct DirectoryEntry: Codable, Sendable, Identifiable {
    public var id: String { pathId }
    public var pathId: String
    public var keyId: UInt8
    public var type: EntryType
    public var value: RomValue
    public var entryQuadletIndex: Int?
    public var rawEntryWord: UInt32?
    public var relativeOffset24: Int32?
    public var targetQuadletIndex: Int?
    public var keyName: String { KeyType.name(for: keyId) }

    public init(pathId: String = UUID().uuidString,
                keyId: UInt8,
                type: EntryType,
                value: RomValue,
                entryQuadletIndex: Int? = nil,
                rawEntryWord: UInt32? = nil,
                relativeOffset24: Int32? = nil,
                targetQuadletIndex: Int? = nil) {
        self.pathId = pathId
        self.keyId = keyId
        self.type = type
        self.value = value
        self.entryQuadletIndex = entryQuadletIndex
        self.rawEntryWord = rawEntryWord
        self.relativeOffset24 = relativeOffset24
        self.targetQuadletIndex = targetQuadletIndex
    }
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
    public var rawROM: Data
    public var busInfoRaw: Data
    public var busInfo: BusInfo
    public var rootDirectoryStartQ: Int
    public var rootDirectory: [DirectoryEntry]
    public var diagnostics: [RomDiagnostic]

    public init(rawROM: Data,
                busInfoRaw: Data,
                busInfo: BusInfo,
                rootDirectoryStartQ: Int,
                rootDirectory: [DirectoryEntry],
                diagnostics: [RomDiagnostic] = []) {
        self.rawROM = rawROM
        self.busInfoRaw = busInfoRaw
        self.busInfo = busInfo
        self.rootDirectoryStartQ = rootDirectoryStartQ
        self.rootDirectory = rootDirectory
        self.diagnostics = diagnostics
    }
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
        self.romRaw = rom.rawROM
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
        guard KeyType(rawValue: e.keyId) == .descriptor else { return nil }
        if case .leafDescriptorText(let s, _) = e.value { return s }
        if case .directory(let d) = e.value {
            return firstDescriptorText(in: d)
        }
        return nil
    }

    private func firstDescriptorText(in entries: [DirectoryEntry]) -> String? {
        for e in entries {
            if case .leafDescriptorText(let s, _) = e.value {
                return s
            }
            if case .directory(let d) = e.value, let nested = firstDescriptorText(in: d) {
                return nested
            }
        }
        return nil
    }
}
