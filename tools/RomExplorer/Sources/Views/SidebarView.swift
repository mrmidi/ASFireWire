import SwiftUI

struct SidebarView: View {
    @EnvironmentObject var store: RomStore

    var body: some View {
        List {
            if let entries = store.entriesToShow {
                if let s = store.summary, (store.selection == nil && !store.showBusInfo) {
                    Section("Summary") {
                        VStack(alignment: .leading, spacing: 4) {
                            if let name = s.vendorName { Text(name) }
                            if let model = s.modelName { Text(model).foregroundStyle(.secondary) }
                            if let mod = s.modalias { Text(mod).font(.system(.caption, design: .monospaced)) }
                        }
                    }
                }
                Section("Bus Info") {
                    if let rom = store.rom {
                        BusInfoView(bus: rom.busInfo)
                            .contentShape(Rectangle())
                            .onTapGesture { store.selectBusInfo() }
                    }
                }
                Section("Root Directory") {
                    DirectoryList(entries: entries)
                }
            } else {
                Text("Open a ROM image to explore").foregroundStyle(.secondary)
            }
        }
        .listStyle(.sidebar)
    }
}

struct DirectoryList: View {
    let entries: [DirectoryEntry]
    var body: some View {
        ForEach(entries.indices, id: \.self) { idx in
            DirectoryEntryRow(entry: entries[idx])
        }
    }
}

struct DirectoryEntryRow: View {
    @EnvironmentObject var store: RomStore
    let entry: DirectoryEntry
    var body: some View {
        switch entry.value {
        case .directory(let sub):
            DisclosureGroup {
                DirectoryList(entries: sub)
            } label: {
                EntryHeader(entry: entry)
            }
            .contentShape(Rectangle())
            .onTapGesture { store.select(entry: entry) }
        default:
            EntryHeader(entry: entry)
                .contentShape(Rectangle())
                .onTapGesture { store.select(entry: entry) }
        }
    }
}

private struct EntryHeader: View {
    let entry: DirectoryEntry
    var body: some View {
        HStack {
            Text(entry.keyName)
            Spacer()
            switch entry.value {
            case .immediate(let v): Text("Immediate 0x\(String(v, radix: 16))").monospaced()
            case .csrOffset(let v): Text(String(format: "CSR 0x%012llx", v)).monospaced()
            case .leafPlaceholder(let off): Text(String(format: "Leaf @+0x%08x", off)).monospaced()
            case .leafDescriptorText(let s, _): Text("Text \"\(s)\"").monospaced()
            case .leafEUI64(let v): Text(String(format: "EUI-64 0x%016llx", v)).monospaced()
            case .leafData(let d): Text("Leaf data (\(d.count) bytes)").monospaced()
            case .directory(let d): Text("Directory (\(d.count) entries)").monospaced()
            }
        }
    }
}

struct BusInfoView: View {
    let bus: BusInfo
    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            Text(String(format: "Vendor: 0x%06x", bus.nodeVendorID))
            Text(String(format: "ChipID: 0x%012llx", bus.chipID))
            HStack {
                Text("irmc: \(bus.irmc)")
                Text("cmc: \(bus.cmc)")
                Text("isc: \(bus.isc)")
                Text("bmc: \(bus.bmc)")
            }
            HStack {
                Text("pmc: \(bus.pmc)")
                Text("adj: \(bus.adj)")
                Text("gen: \(bus.gen)")
                Text("spd: \(bus.linkSpd)")
            }
            HStack {
                Text("cyc-clk-acc: \(bus.cycClkAcc)")
                Text("maxRec: \(bus.maxRec)")
            }
        }
        .font(.system(.body, design: .monospaced))
    }
}
