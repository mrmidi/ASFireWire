import Foundation
import SwiftUI

@MainActor
final class RomStore: ObservableObject {
    @Published var rom: RomTree?
    @Published var error: String?
    @Published var selection: DirectoryEntry?
    @Published var showBusInfo: Bool = false
    @Published var showInterpreted: Bool = true

    // Summarized info for UI (vendor/model names, modalias, units)
    var summary: RomSummary? {
        guard let rom else { return nil }
        return Summarizer.summarize(tree: rom)
    }

    func open(url: URL) {
        do {
            rom = try RomParser.parse(fileURL: url)
            error = nil
            selection = nil
            showBusInfo = true
        } catch {
            rom = nil
            self.error = String(describing: error)
        }
    }

    func selectBusInfo() { showBusInfo = true; selection = nil }
    func select(entry: DirectoryEntry) { selection = entry; showBusInfo = false }

    var entriesToShow: [DirectoryEntry]? {
        guard let rom else { return nil }
        if showInterpreted {
            return RomInterpreter.interpretRoot(rom.rootDirectory)
        } else {
            return rom.rootDirectory
        }
    }

    var selectionDescription: String? {
        guard let sel = selection else { return nil }
        var out: [String] = []
        out.append("Key: \(sel.keyName) (0x\(String(sel.keyId, radix: 16))) type: \(sel.type)")
        switch sel.value {
        case .immediate(let v): out.append(String(format: "Immediate: 0x%08x", v))
        case .csrOffset(let v): out.append(String(format: "CSR: 0x%012llx", v))
    case .leafPlaceholder(let off): out.append(String(format: "Leaf offset (relative): 0x%08x", off))
    case .leafDescriptorText(let s, _): out.append("Descriptor text: \"\(s)\"")
        case .leafEUI64(let v): out.append(String(format: "EUI-64: 0x%016llx", v))
    case .leafData(let d): out.append("Leaf data bytes: \(d.count)")
        case .directory(let d): out.append("Directory entries: \(d.count)")
        }
        return out.joined(separator: "\n")
    }
}
