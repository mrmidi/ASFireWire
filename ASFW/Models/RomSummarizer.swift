//
//  RomSummarizer.swift
//  ASFW
//
//  ROM summary extraction for display
//  Integrated from RomExplorer tool
//

import Foundation

enum Summarizer {
    static func summarize(tree: RomTree) -> RomSummary {
        let root = RomDirectoryView(rom: tree)
        var vendorId: UInt32?
        var modelId: UInt32?
        var vendorName: String?
        var modelName: String?

        var i = 0
        while let idx = root.findIndex(key: .vendor, type: .immediate, startAt: i) {
            vendorId = root.getImmediate(at: idx)
            vendorName = vendorName ?? root.getDescriptorTextAdjacent(to: idx)
            i = idx + 1
        }
        i = 0
        while let idx = root.findIndex(key: .model, type: .immediate, startAt: i) {
            modelId = root.getImmediate(at: idx)
            modelName = modelName ?? root.getDescriptorTextAdjacent(to: idx)
            i = idx + 1
        }

        var units: [UnitInfo] = []
        collectUnits(in: tree.rootDirectory, into: &units)

        // Some devices publish model_id only in a unit directory: real hardware
        // (RME Fireface 400/800, IEEE 1212 unit directory key 0x17) has no root
        // model at all, and the driver's catalog matches those from the unit.
        // A root model always wins, and the source is reported, so an absent
        // root key is not shown as "the device advertises nothing".
        var modelIdSource: RomModelIdSource?
        if modelId != nil {
            modelIdSource = .rootDirectory
        } else if let fromUnit = units.first(where: { $0.modelId != nil }) {
            modelId = fromUnit.modelId
            modelName = modelName ?? fromUnit.modelName
            modelIdSource = .unitDirectory
        }

        let sp = units.first?.specifierId ?? 0
        let ver = units.first?.version ?? 0
        let ven = vendorId ?? 0
        let mo = modelId ?? 0
        let modalias = String(format: "ieee1394:ven%08Xmo%08Xsp%08Xver%08X", ven, mo, sp, ver)

        return RomSummary(vendorId: vendorId,
                          vendorName: vendorName,
                          modelId: modelId,
                          modelName: modelName,
                          modelIdSource: modelIdSource,
                          units: units,
                          modalias: modalias)
    }

    private static func collectUnits(in entries: [DirectoryEntry], into out: inout [UnitInfo]) {
        for (idx, e) in entries.enumerated() {
            guard KeyType(rawValue: e.keyId) == .unit, case .directory(let sub) = e.value else { continue }
            var info = UnitInfo()
            let v = RomDirectoryView(romRaw: Data(), entries: sub)
            if let si = v.findIndex(key: .specifierId, type: .immediate), let sv = v.getImmediate(at: si) { info.specifierId = sv }
            if let vi = v.findIndex(key: .version, type: .immediate), let vv = v.getImmediate(at: vi) { info.version = vv }
            if let mi = v.findIndex(key: .model, type: .immediate) {
                info.modelId = v.getImmediate(at: mi)
                info.modelName = v.getDescriptorTextAdjacent(to: mi) ?? info.modelName
            }
            out.append(info)
            collectUnits(in: sub, into: &out)
        }
    }
}

/// The Raw tab's export: the cached ROM bytes as quadlets and as a byte dump,
/// independent of how the tree decoded them, so a report can be pasted or
/// attached and still be the evidence itself rather than a transcription.
/// Covered by RomRawExportTests, which round-trips both representations back to
/// the bytes that were read from the device.
enum RomRawExport {
    static func text(for data: Data) -> String {
        var lines = [
            "ASFW Config ROM",
            "size: \(data.count) bytes (\(data.count / 4) quadlets)",
            "",
            "quadlets (big-endian wire order):",
        ]
        for index in 0..<(data.count / 4) {
            lines.append(quadletLine(index: index, in: data))
        }
        lines.append("")
        lines.append("hex dump:")
        lines.append(hexDump(data))
        return lines.joined(separator: "\n")
    }

    static func quadletLine(index: Int, in data: Data) -> String {
        let offset = index * 4
        guard offset + 4 <= data.count else {
            return String(format: "q%-3d <truncated>", index)
        }
        let q = UInt32(data[offset]) << 24 |
                UInt32(data[offset + 1]) << 16 |
                UInt32(data[offset + 2]) << 8 |
                UInt32(data[offset + 3])
        return String(format: "q%-3d  0x%08X", index, q)
    }

    static func hexDump(_ data: Data) -> String {
        var result = ""
        let bytesPerLine = 16

        for offset in stride(from: 0, to: data.count, by: bytesPerLine) {
            result += String(format: "%08X: ", offset)
            for i in 0..<bytesPerLine {
                let idx = offset + i
                if idx < data.count {
                    result += String(format: "%02X ", data[idx])
                } else {
                    result += "   "
                }
                if i == 7 { result += " " }
            }
            result += " "
            for i in 0..<bytesPerLine {
                let idx = offset + i
                if idx < data.count {
                    let b = data[idx]
                    result += (32..<127).contains(b) ? String(Character(UnicodeScalar(Int(b))!)) : "."
                }
            }
            result += "\n"
        }

        return result
    }
}
