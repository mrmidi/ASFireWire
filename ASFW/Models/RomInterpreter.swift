//
//  RomInterpreter.swift
//  ASFW
//
//  ROM directory interpreter for filtering entries
//  Integrated from RomExplorer tool
//

import Foundation

enum DirectoryKind {
    case root
    case unit
    case feature
    case instance
    case vendor
    case generic
}

struct RomInterpreter {
    static func interpretRoot(_ entries: [DirectoryEntry]) -> [DirectoryEntry] {
        return interpret(entries, kind: .root)
    }

    private static func interpret(_ entries: [DirectoryEntry], kind: DirectoryKind) -> [DirectoryEntry] {
        let allowed: Set<UInt8> = allowedKeys(for: kind)
        var out: [DirectoryEntry] = []
        out.reserveCapacity(entries.count)
        for e in entries {
            let k = e.keyId
            let nameKnown = KeyType(rawValue: k) != nil
            if !nameKnown { continue }
            if !allowed.contains(k) { continue }
            switch (KeyType(rawValue: k), e.value) {
            case (.some(.unit), .directory(let sub)):
                let child = interpret(sub, kind: .unit)
                out.append(DirectoryEntry(keyId: e.keyId, type: e.type, value: .directory(child)))
            case (.some(.feature), .directory(let sub)):
                let child = interpret(sub, kind: .feature)
                out.append(DirectoryEntry(keyId: e.keyId, type: e.type, value: .directory(child)))
            case (.some(.instance), .directory(let sub)):
                let child = interpret(sub, kind: .instance)
                out.append(DirectoryEntry(keyId: e.keyId, type: e.type, value: .directory(child)))
            case (.some(.vendor), .directory(let sub)):
                let child = interpret(sub, kind: .vendor)
                out.append(DirectoryEntry(keyId: e.keyId, type: e.type, value: .directory(child)))
            case (_, .directory(let sub)):
                // Keep known-key directories, interpret generically
                let child = interpret(sub, kind: .generic)
                out.append(DirectoryEntry(keyId: e.keyId, type: e.type, value: .directory(child)))
            default:
                out.append(e)
            }
        }
        return out
    }

    private static func allowedKeys(for kind: DirectoryKind) -> Set<UInt8> {
        func ks(_ arr: [KeyType]) -> Set<UInt8> { Set(arr.map { $0.rawValue }) }
        switch kind {
        case .root:
            return ks([.busDependentInfo, .vendor, .hardwareVersion, .module, .nodeCapabilities, .instance, .unit, .model, .dependentInfo, .descriptor, .eui64])
        case .unit:
            return ks([.vendor, .model, .specifierId, .version, .dependentInfo, .feature, .descriptor, .unitLocation])
        case .feature:
            return ks([.specifierId, .version, .dependentInfo, .descriptor])
        case .instance:
            return ks([.vendor, .keyword, .feature, .instance, .unit, .model, .dependentInfo, .descriptor])
        case .vendor:
            return ks([.descriptor, .model, .dependentInfo])
        case .generic:
            return ks([.descriptor, .dependentInfo, .vendor, .model, .specifierId, .version, .unit, .feature])
        }
    }
}
