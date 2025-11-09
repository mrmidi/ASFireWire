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
        let bib = try cache.busInfoRaw()
        let busInfo = try parseBusInfo(raw: bib)
        let root = try readDirectory(cache: cache, startQ: cache.rootDirectoryStartQ)
        return RomTree(busInfoRaw: bib, busInfo: busInfo, rootDirectory: root)
    }

    private static func parseBusInfo(raw: Data) throws -> BusInfo {
        guard raw.count >= 16 else { throw RomError.invalidData("bus-info < 16 bytes") }
        let busName = raw.withUnsafeBytes { $0.load(as: UInt32.self).bigEndian }
        guard busName == 0x31333934 else { throw RomError.invalidData("bus_name mismatch") }

        let meta1 = raw.withUnsafeBytes { $0.load(fromByteOffset: 4, as: UInt32.self).bigEndian }
        let meta2 = raw.withUnsafeBytes { $0.load(fromByteOffset: 8, as: UInt32.self).bigEndian }
        let meta3 = raw.withUnsafeBytes { $0.load(fromByteOffset: 12, as: UInt32.self).bigEndian }

        let irmc = (meta1 & 0x8000_0000) >> 31
        let cmc  = (meta1 & 0x4000_0000) >> 30
        let isc  = (meta1 & 0x2000_0000) >> 29
        let bmc  = (meta1 & 0x1000_0000) >> 28
        let cyc  = (meta1 & 0x00ff_0000) >> 16
        let maxRec = (meta1 & 0x0000_f000) >> 12

        let pmc: UInt32 = (meta1 & 0x0800_0000) >> 27 // IEEE 1394a:2000
        let gen: UInt32 = (meta1 & 0x0000_00c0) >> 6
        let linkSpd: UInt32 = (meta1 & 0x0000_0007)
        let adj: UInt32 = (meta1 & 0x0400_0000) >> 26 // 1394:2008

        let nodeVendor = (meta2 & 0xffff_ff00) >> 8
        let chipID = (UInt64(meta2 & 0x0000_00ff) << 32) | UInt64(meta3)

        return BusInfo(irmc: irmc, cmc: cmc, isc: isc, bmc: bmc,
                       cycClkAcc: cyc, maxRec: maxRec,
                       pmc: pmc, gen: gen, linkSpd: linkSpd, adj: adj,
                       nodeVendorID: nodeVendor, chipID: chipID)
    }

    private static func leafPlaceholder(base: Int) -> RomValue { .leafPlaceholder(offset: base) }

    private static func readLeafSafe(cache: RomCache, leafStartQ: Int, keyId: UInt8) -> RomValue {
        // length/crc
        if leafStartQ >= cache.quadletCount {
            logger.warning("Leaf OOB leafStartQ=\(leafStartQ) totalQ=\(cache.quadletCount)")
            return .leafPlaceholder(offset: leafStartQ * 4)
        }
        let meta = (try? cache.quadlet(at: leafStartQ)) ?? 0
        let quadlets = Int((meta & 0xffff_0000) >> 16)
        let payloadQ = leafStartQ + 1
        if payloadQ > cache.quadletCount {
            logger.warning("Leaf payload OOB payloadQ=\(payloadQ) quadlets=\(quadlets) totalQ=\(cache.quadletCount)")
            return .leafPlaceholder(offset: leafStartQ * 4)
        }
        let payload = (try? cache.readBytes(quadletStart: payloadQ, quadletLength: quadlets)) ?? Data()

        switch KeyType(rawValue: keyId) {
        case .descriptor:
            // Descriptor leaf: first 4 bytes are [type:8][specifier_id:24]
            guard payload.count >= 4 else { return .leafPlaceholder(offset: leafStartQ*4) }
            let descHdr = payload.withUnsafeBytes { $0.load(as: UInt32.self).bigEndian }
            let descType = Int((descHdr & 0xFF00_0000) >> 24)
            let specId = Int(descHdr & 0x00FF_FFFF)
            let remain = payload.dropFirst(4)
            // Textual descriptor when specifier_id==0 and type==0 per IEEE-1212
            if specId == 0, descType == 0, remain.count >= 4 {
                let textHdr = remain.withUnsafeBytes { $0.load(as: UInt32.self).bigEndian }
                let width = Int((textHdr & 0xF000_0000) >> 28) // 0: 8-bit, 1: 16-bit
                let textBytes = remain.dropFirst(4)
                if width == 0 {
                    // 8-bit bytes, typically ASCII or ISO-8859-1
                    let rawData = Data(textBytes)
                    if let s = String(bytes: rawData, encoding: .ascii) ?? String(bytes: rawData, encoding: .isoLatin1) {
                        let trimmed = s.split(separator: "\0").first.map(String.init) ?? s
                        if !trimmed.isEmpty { return .leafDescriptorText(trimmed, rawData) }
                    }
                } else if width == 1 {
                    // 16-bit, try UTF-16BE
                    let rawData = Data(textBytes)
                    if let s = String(data: rawData, encoding: .utf16BigEndian) {
                        let trimmed = s.split(separator: "\0").first.map(String.init) ?? s
                        if !trimmed.isEmpty { return .leafDescriptorText(trimmed, rawData) }
                    }
                } else {
                    // Try UTF-32BE for wider widths
                    let rawData = Data(textBytes)
                    if let s = String(data: rawData, encoding: .utf32BigEndian) {
                        let trimmed = s.split(separator: "\0").first.map(String.init) ?? s
                        if !trimmed.isEmpty { return .leafDescriptorText(trimmed, rawData) }
                    }
                }
            }
            return .leafData(payload)
        case .eui64:
            if payload.count >= 8 {
                let hi = payload.withUnsafeBytes { $0.load(as: UInt32.self).bigEndian }
                let lo = payload.withUnsafeBytes { $0.load(fromByteOffset: 4, as: UInt32.self).bigEndian }
                return .leafEUI64((UInt64(hi) << 32) | UInt64(lo))
            }
            return .leafData(payload)
        default:
            return .leafData(payload)
        }
    }

    private static func readDirectory(cache: RomCache, startQ: Int) throws -> [DirectoryEntry] {
        let meta = try cache.quadlet(at: startQ)
        let quadlets = Int((meta & 0xffff_0000) >> 16)
        let entryCount = quadlets
        let maxReadable = max(0, cache.quadletCount - (startQ + 1))
        let count = min(entryCount, maxReadable)
        logger.info("Dir startQ=\(startQ) lenQ=\(entryCount) clamp=\(count) quadlets=\(cache.quadletCount)")
        var result: [DirectoryEntry] = []
        var i = 0
        while i < count {
            let qIndex = startQ + 1 + i
            let word = try cache.quadlet(at: qIndex)
            let keyType = UInt8((word & 0xC000_0000) >> 30)
            let keyId = UInt8((word & 0x3F00_0000) >> 24)
            let value = word & 0x00FF_FFFF
            guard let et = EntryType(rawValue: keyType) else { throw RomError.unsupported("entry type \(keyType)") }
            let parsedValue: RomValue
            switch et {
            case .immediate:
                parsedValue = .immediate(value)
            case .csrOffset:
                parsedValue = .csrOffset(0xffff_f000_0000 + UInt64(value) * 4)
            case .leaf:
                let leafStartQ = startQ + Int(value)
                parsedValue = readLeafSafe(cache: cache, leafStartQ: leafStartQ, keyId: keyId)
            case .directory:
                let childStartQ = startQ + Int(value)
                if childStartQ + 1 <= cache.quadletCount {
                    let dir = try readDirectory(cache: cache, startQ: childStartQ)
                    parsedValue = .directory(dir)
                } else {
                    logger.warning("Subdir OOB childStartQ=\(childStartQ) totalQ=\(cache.quadletCount)")
                    parsedValue = .directory([])
                }
            }
            result.append(DirectoryEntry(keyId: keyId, type: et, value: parsedValue))
            i += 1
        }
        return result
    }
}
