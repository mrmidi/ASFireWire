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

        let sp = units.first?.specifierId ?? 0
        let ver = units.first?.version ?? 0
        let ven = vendorId ?? 0
        let mo = modelId ?? 0
        let modalias = String(format: "ieee1394:ven%08Xmo%08Xsp%08Xver%08X", ven, mo, sp, ver)

        return RomSummary(vendorId: vendorId,
                          vendorName: vendorName,
                          modelId: modelId,
                          modelName: modelName,
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
