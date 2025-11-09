import Foundation

// Apple-style directory utilities over RomTree
// - findIndex with optional type filter
// - typed getters: immediate, leaf data (as Data), directory, CSR offset
// - optional adjacency-based descriptor retrieval at index+1

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

    public init(romRaw: Data, entries: [DirectoryEntry]) {
        self.romRaw = romRaw
        self.entries = entries
    }

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
