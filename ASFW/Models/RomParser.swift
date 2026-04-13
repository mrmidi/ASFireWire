//
//  RomParser.swift
//  ASFW
//
//  Config ROM parser implementation
//  Integrated from RomExplorer tool
//

import Foundation
import os

private let logger = Logger(subsystem: Bundle.main.bundleIdentifier ?? "ASFW", category: "RomParser")

public struct RomParser {
    public static func parse(fileURL: URL) throws -> RomTree {
        let fileData = try Data(contentsOf: fileURL)
        return try parse(data: fileData)
    }

    public static func parse(data: Data) throws -> RomTree {
        let cache = try RomCache(fileData: data)
        var diagnostics: [RomDiagnostic] = []
        let bib = try parseBusInfo(cache: cache, diagnostics: &diagnostics)
        var visitedDirectories = Set<Int>()
        let root = try readDirectory(cache: cache,
                                     startQ: cache.rootDirectoryStartQ,
                                     path: "root",
                                     depth: 0,
                                     visitedDirectories: &visitedDirectories,
                                     diagnostics: &diagnostics)
        let busInfoRaw = (try? cache.busInfoRaw()) ?? Data()
        return RomTree(rawROM: cache.normalizedData,
                       busInfoRaw: busInfoRaw,
                       busInfo: bib,
                       rootDirectoryStartQ: cache.rootDirectoryStartQ,
                       rootDirectory: root,
                       diagnostics: diagnostics)
    }

    private static func parseBusInfo(cache: RomCache, diagnostics: inout [RomDiagnostic]) throws -> BusInfo {
        let q0 = try cache.quadlet(at: 0)
        let busInfoLength = Int((q0 >> 24) & 0xff)
        let crcLength = UInt8((q0 >> 16) & 0xff)
        let crc = UInt16(q0 & 0xffff)

        if busInfoLength < 4 {
            diagnostics.append(.init(severity: .warning,
                                     message: "Bus info length is \(busInfoLength) quadlets; GUID fields may be incomplete."))
        }

        guard cache.quadletCount >= 1 + max(busInfoLength, 0) else {
            throw RomError.invalidData("BIB exceeds ROM length")
        }

        let q1 = busInfoLength >= 1 ? try cache.quadlet(at: 1) : 0
        let q2 = busInfoLength >= 2 ? try cache.quadlet(at: 2) : 0
        let q3 = busInfoLength >= 3 ? try cache.quadlet(at: 3) : 0
        let q4 = busInfoLength >= 4 ? try cache.quadlet(at: 4) : 0

        if q1 != 0x3133_3934 {
            diagnostics.append(.init(severity: .warning,
                                     message: String(format: "Unexpected bus_name quadlet: 0x%08X (expected 0x31333934 '1394').", q1)))
        }

        let busOptions = decodeBusOptions(q2)
        let header = ConfigROMHeader(busInfoLength: UInt8(clamping: busInfoLength),
                                     crcLength: crcLength,
                                     crc: crc,
                                     rawQuadlet: q0)
        return BusInfo(header: header, busName: q1, busOptions: busOptions, guidHigh: q3, guidLow: q4)
    }

    private static func decodeBusOptions(_ raw: UInt32) -> BusOptions {
        BusOptions(raw: raw,
                   irmc: (raw & 0x8000_0000) != 0,
                   cmc: (raw & 0x4000_0000) != 0,
                   isc: (raw & 0x2000_0000) != 0,
                   bmc: (raw & 0x1000_0000) != 0,
                   pmc: (raw & 0x0800_0000) != 0,
                   cycClkAcc: UInt8((raw & 0x00ff_0000) >> 16),
                   maxRec: UInt8((raw & 0x0000_f000) >> 12),
                   maxRom: UInt8((raw & 0x0000_0300) >> 8),
                   generation: UInt8((raw & 0x0000_00f0) >> 4),
                   linkSpd: UInt8(raw & 0x0000_0007))
    }

    private static func signExtend24(_ raw: UInt32) -> Int32 {
        var value = Int32(raw & 0x00ff_ffff)
        if (value & 0x0080_0000) != 0 {
            value |= ~0x00ff_ffff
        }
        return value
    }

    private static func readLeafSafe(cache: RomCache,
                                     leafStartQ: Int,
                                     keyId: UInt8,
                                     diagnostics: inout [RomDiagnostic]) -> RomValue {
        guard leafStartQ >= 0 else {
            diagnostics.append(.init(severity: .warning, message: "Leaf offset resolved to negative quadlet index."))
            return .leafPlaceholder(offset: leafStartQ * 4)
        }
        guard leafStartQ < cache.quadletCount else {
            logger.warning("Leaf OOB leafStartQ=\(leafStartQ) totalQ=\(cache.quadletCount)")
            diagnostics.append(.init(severity: .warning,
                                     message: "Leaf target q\(leafStartQ) is outside ROM (\(cache.quadletCount) quadlets)."))
            return .leafPlaceholder(offset: leafStartQ * 4)
        }

        let meta = (try? cache.quadlet(at: leafStartQ)) ?? 0
        let bodyQuadlets = Int((meta & 0xffff_0000) >> 16)
        let totalQuadlets = 1 + bodyQuadlets
        let endQ = leafStartQ + totalQuadlets
        guard endQ <= cache.quadletCount else {
            diagnostics.append(.init(severity: .warning,
                                     message: "Leaf at q\(leafStartQ) truncated (needs \(totalQuadlets) quadlets)."))
            return .leafPlaceholder(offset: leafStartQ * 4)
        }

        let payload = (try? cache.readBytes(quadletStart: leafStartQ + 1, quadletLength: bodyQuadlets)) ?? Data()

        if KeyType(rawValue: keyId) == .descriptor,
           let parsed = parseTextDescriptorLeaf(cache: cache,
                                                leafStartQ: leafStartQ,
                                                bodyQuadlets: bodyQuadlets,
                                                diagnostics: &diagnostics) {
            return parsed
        }

        if KeyType(rawValue: keyId) == .eui64, payload.count >= 8 {
            let hi = payload.withUnsafeBytes { $0.load(as: UInt32.self).bigEndian }
            let lo = payload.withUnsafeBytes { $0.load(fromByteOffset: 4, as: UInt32.self).bigEndian }
            return .leafEUI64((UInt64(hi) << 32) | UInt64(lo))
        }

        return .leafData(payload)
    }

    private static func parseTextDescriptorLeaf(cache: RomCache,
                                                leafStartQ: Int,
                                                bodyQuadlets: Int,
                                                diagnostics: inout [RomDiagnostic]) -> RomValue? {
        guard bodyQuadlets >= 2 else { return nil }

        let descHeader = (try? cache.quadlet(at: leafStartQ + 1)) ?? 0
        let descriptorType = UInt8((descHeader >> 24) & 0xff)
        let specifierID = descHeader & 0x00ff_ffff

        // IEEE 1212 textual descriptor leaf we support in UI: type=0, specifier_ID=0 (minimal ASCII form).
        guard descriptorType == 0, specifierID == 0 else { return nil }

        let textMeta = (try? cache.quadlet(at: leafStartQ + 2)) ?? 0
        guard textMeta == 0 else {
            diagnostics.append(.init(severity: .info,
                                     message: String(format: "Descriptor leaf at q%d uses non-minimal text encoding header 0x%08X; showing raw bytes.", leafStartQ, textMeta)))
            return nil
        }

        let textQuadlets = bodyQuadlets - 2
        guard textQuadlets >= 0 else { return nil }
        let rawText = (try? cache.readBytes(quadletStart: leafStartQ + 3, quadletLength: textQuadlets)) ?? Data()
        guard !rawText.isEmpty else { return .leafDescriptorText("", Data()) }

        let trimmedBytes = Array(rawText.prefix { $0 != 0 })
        if let str = String(bytes: trimmedBytes, encoding: .ascii) {
            return .leafDescriptorText(str, Data(trimmedBytes))
        }
        if let str = String(bytes: trimmedBytes, encoding: .isoLatin1) {
            return .leafDescriptorText(str, Data(trimmedBytes))
        }

        return nil
    }

    private static func readDirectory(cache: RomCache,
                                      startQ: Int,
                                      path: String,
                                      depth: Int,
                                      visitedDirectories: inout Set<Int>,
                                      diagnostics: inout [RomDiagnostic]) throws -> [DirectoryEntry] {
        guard startQ >= 0 else { return [] }
        guard startQ < cache.quadletCount else {
            diagnostics.append(.init(severity: .warning,
                                     message: "Directory target q\(startQ) is outside ROM."))
            return []
        }
        guard depth < 24 else {
            diagnostics.append(.init(severity: .warning,
                                     message: "Directory recursion depth exceeded safety cap at q\(startQ)."))
            return []
        }
        guard visitedDirectories.insert(startQ).inserted else {
            diagnostics.append(.init(severity: .warning,
                                     message: "Directory cycle detected at q\(startQ)."))
            return []
        }
        defer { visitedDirectories.remove(startQ) }

        let meta = try cache.quadlet(at: startQ)
        let entryCount = Int((meta & 0xffff_0000) >> 16)
        let available = max(0, cache.quadletCount - (startQ + 1))
        let clampedEntryCount = min(entryCount, available)

        if clampedEntryCount < entryCount {
            diagnostics.append(.init(severity: .warning,
                                     message: "Directory at q\(startQ) truncated: header says \(entryCount) entries, only \(clampedEntryCount) available."))
        }

        logger.info("Dir startQ=\(startQ) entries=\(entryCount) clamp=\(clampedEntryCount) quadlets=\(cache.quadletCount)")

        var result: [DirectoryEntry] = []
        result.reserveCapacity(clampedEntryCount)

        for index in 0..<clampedEntryCount {
            let qIndex = startQ + 1 + index
            let word = try cache.quadlet(at: qIndex)
            let typeBits = UInt8((word & 0xC000_0000) >> 30)
            let keyId = UInt8((word & 0x3F00_0000) >> 24)
            let value24 = word & 0x00ff_ffff

            guard let et = EntryType(rawValue: typeBits) else {
                diagnostics.append(.init(severity: .warning,
                                         message: "Unsupported entry type \(typeBits) at q\(qIndex)."))
                continue
            }

            let rel24 = signExtend24(value24)
            let targetQ = qIndex + Int(rel24)
            let pathId = "\(path)/\(index)-k\(String(format: "%02X", keyId))-t\(typeBits)@q\(qIndex)"

            let parsedValue: RomValue
            switch et {
            case .immediate:
                parsedValue = .immediate(value24)
            case .csrOffset:
                parsedValue = .csrOffset(0xffff_f000_0000 + UInt64(value24) * 4)
            case .leaf:
                parsedValue = readLeafSafe(cache: cache,
                                           leafStartQ: targetQ,
                                           keyId: keyId,
                                           diagnostics: &diagnostics)
            case .directory:
                let childPath = "\(path)/dir\(index)"
                let child = try readDirectory(cache: cache,
                                              startQ: targetQ,
                                              path: childPath,
                                              depth: depth + 1,
                                              visitedDirectories: &visitedDirectories,
                                              diagnostics: &diagnostics)
                parsedValue = .directory(child)
            }

            result.append(DirectoryEntry(pathId: pathId,
                                         keyId: keyId,
                                         type: et,
                                         value: parsedValue,
                                         entryQuadletIndex: qIndex,
                                         rawEntryWord: word,
                                         relativeOffset24: et == .immediate ? nil : rel24,
                                         targetQuadletIndex: et == .immediate ? nil : targetQ))
        }

        return result
    }
}
