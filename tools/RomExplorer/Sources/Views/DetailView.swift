import SwiftUI

struct DetailView: View {
    @EnvironmentObject var store: RomStore

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            if let rom = store.rom {
                if store.showBusInfo {
                        GroupBox("Bus Info Raw (Hex)") {
                        HexView(data: rom.busInfoRaw)
                            .frame(maxWidth: .infinity, maxHeight: 400)
                    }
                } else if let selection = store.selectionDescription {
                    VStack(alignment: .leading, spacing: 8) {
                        GroupBox("Selection") {
                            Text(selection)
                                .font(.system(.body, design: .monospaced))
                                .frame(maxWidth: .infinity, alignment: .leading)
                        }
                        if let sel = store.selection {
                            switch sel.value {
                            case .leafDescriptorText(_, let d):
                                GroupBox("Leaf Payload (Hex)") { HexView(data: d).frame(maxWidth: .infinity, maxHeight: 200) }
                            case .leafData(let d):
                                GroupBox("Leaf Payload (Hex)") { HexView(data: d).frame(maxWidth: .infinity, maxHeight: 200) }
                            default: EmptyView()
                            }
                        }
                    }
                } else if let s = store.summary {
                    GroupBox("Summary") {
                        VStack(alignment: .leading, spacing: 6) {
                            if let vn = s.vendorName { Text("Vendor: \(vn)") }
                            if let vId = s.vendorId { Text(String(format: "Vendor ID: 0x%08X", vId)).monospaced() }
                            if let mn = s.modelName { Text("Model: \(mn)") }
                            if let mId = s.modelId { Text(String(format: "Model ID: 0x%08X", mId)).monospaced() }
                            if let mod = s.modalias {
                                HStack {
                                    Text(mod).font(.system(.body, design: .monospaced)).textSelection(.enabled)
                                    Spacer()
                                    Button("Copy Modalias") { NSPasteboard.general.clearContents(); NSPasteboard.general.setString(mod, forType: .string) }
                                }
                            }
                            if !s.units.isEmpty {
                                Divider()
                                Text("Units: \(s.units.count)")
                                ForEach(Array(s.units.enumerated()), id: \.offset) { _, u in
                                    HStack(spacing: 12) {
                                        if let name = u.modelName { Text(name) }
                                        if let sp = u.specifierId { Text(String(format: "sp=0x%08X", sp)).monospaced() }
                                        if let ver = u.version { Text(String(format: "ver=0x%08X", ver)).monospaced() }
                                    }
                                }
                            }
                        }
                        .frame(maxWidth: .infinity, alignment: .leading)
                    }
                } else {
                    Text("Select an entry to preview").foregroundStyle(.secondary)
                }
            } else {
                Text("No ROM loaded").foregroundStyle(.secondary)
            }
        }
        .padding()
    }
}

struct HexView: View {
    let data: Data
    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack {
                Spacer()
                Button("Copy Hex") { NSPasteboard.general.clearContents(); NSPasteboard.general.setString(Self.formatHexDump(data), forType: .string) }
            }
            ScrollView {
                Text(Self.formatHexDump(data))
                    .font(.system(.caption, design: .monospaced))
                    .textSelection(.enabled)
                    .frame(maxWidth: .infinity, alignment: .leading)
            }
        }
    }

    static func formatHexDump(_ data: Data) -> String {
        var out: [String] = []
        let bytes = [UInt8](data)
        for chunkStart in stride(from: 0, to: bytes.count, by: 16) {
            let end = min(chunkStart + 16, bytes.count)
            let slice = bytes[chunkStart..<end]
            let hex = slice.map { String(format: "%02x", $0) }.joined(separator: " ")
            let ascii = slice.map { b -> String in
                (0x20...0x7e).contains(Int(b)) ? String(UnicodeScalar(b)) : "."
            }.joined()
            let addr = String(format: "%08x", chunkStart)
            let paddedHex = hex.padding(toLength: 16 * 3 - 1, withPad: " ", startingAt: 0)
            out.append("\(addr)  \(paddedHex)  |\(ascii)|")
        }
        return out.joined(separator: "\n")
    }
}
