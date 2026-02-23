//
//  ROMExplorerView.swift
//  ASFW
//
//  Config ROM Explorer (spec-aware Swift parsing in app)
//

import SwiftUI

struct ROMExplorerView: View {
    @ObservedObject var viewModel: RomExplorerViewModel

    @State private var selectedNodeId: UInt8?
    @State private var autoRefreshEnabled = false
    @State private var autoRefreshTimer: Timer?
    @State private var showAdvancedBIB = false
    @State private var treeSelectionID: String?

    var body: some View {
        HSplitView {
            sidebar
                .frame(minWidth: 260, idealWidth: 300)

            detail
                .frame(minWidth: 520)
        }
        .navigationTitle("ROM Explorer")
        .toolbar {
            ToolbarItemGroup(placement: .automatic) {
                Toggle("Auto-refresh", isOn: $autoRefreshEnabled)
                    .toggleStyle(.switch)
                    .onChange(of: autoRefreshEnabled) { _, enabled in
                        enabled ? startAutoRefresh() : stopAutoRefresh()
                    }

                Toggle("Interpreted", isOn: $viewModel.showInterpreted)
                    .help("Filter the tree to common/known Config ROM keys")
            }
        }
        .onAppear {
            viewModel.refreshAvailableNodes()
            selectedNodeId = viewModel.selectedNode?.nodeId
        }
        .onDisappear {
            stopAutoRefresh()
        }
    }

    private var sidebar: some View {
        VStack(alignment: .leading, spacing: 0) {
            HStack {
                Text("Nodes")
                    .font(.headline)
                    .padding(.leading, 12)
                Spacer()
                Button {
                    viewModel.refreshTopology()
                } label: {
                    Label("Refresh", systemImage: "arrow.clockwise")
                }
                .buttonStyle(.borderless)
                .padding(.trailing, 12)
            }
            .frame(height: 44)
            .background(Color(nsColor: .controlBackgroundColor))

            Divider()

            if viewModel.availableNodes.isEmpty {
                VStack(spacing: 10) {
                    Image(systemName: "network")
                        .font(.title2)
                        .foregroundStyle(.secondary)
                    Text("No topology nodes available")
                        .foregroundStyle(.secondary)
                    Button("Refresh Topology") {
                        viewModel.refreshTopology()
                    }
                }
                .frame(maxWidth: .infinity, maxHeight: .infinity)
                .padding()
            } else {
                List(viewModel.availableNodes, id: \.nodeId, selection: $selectedNodeId) { node in
                    ROMNodeRow(node: node,
                               isSelected: selectedNodeId == node.nodeId,
                               hasCachedROM: viewModel.selectedNode?.nodeId == node.nodeId && viewModel.rom != nil)
                        .tag(node.nodeId)
                }
                .listStyle(.sidebar)
                .onChange(of: selectedNodeId) { _, newValue in
                    let node = viewModel.availableNodes.first(where: { $0.nodeId == newValue })
                    viewModel.selectNode(node)
                    if node != nil {
                        viewModel.loadROMFromSelectedNodeCache()
                    }
                }
            }
        }
    }

    private var detail: some View {
        VStack(alignment: .leading, spacing: 0) {
            headerBar
            Divider()
            statusArea
            Divider()

            if viewModel.isLoading && viewModel.rom == nil {
                VStack(spacing: 12) {
                    ProgressView()
                    Text("Loading Config ROM...")
                        .foregroundStyle(.secondary)
                }
                .frame(maxWidth: .infinity, maxHeight: .infinity)
            } else if let rom = viewModel.rom {
                ROMExplorerTabsView(rom: rom,
                                    summary: viewModel.summary,
                                    showInterpreted: viewModel.showInterpreted,
                                    showAdvancedBIB: $showAdvancedBIB,
                                    treeSelectionID: $treeSelectionID)
            } else {
                VStack(spacing: 12) {
                    Image(systemName: "memorychip")
                        .font(.system(size: 38))
                        .foregroundStyle(.secondary)
                    Text(viewModel.selectedNode == nil ? "Select a node to inspect its Config ROM" : "No parsed ROM available yet")
                        .foregroundStyle(.secondary)
                    if viewModel.selectedNode != nil {
                        Text("Use 'Read ROM' to ask the driver to fetch the ROM, then this view parses and explains it in Swift.")
                            .font(.callout)
                            .foregroundStyle(.tertiary)
                            .multilineTextAlignment(.center)
                            .frame(maxWidth: 420)
                    }
                }
                .frame(maxWidth: .infinity, maxHeight: .infinity)
                .padding()
            }
        }
    }

    private var headerBar: some View {
        HStack(spacing: 12) {
            VStack(alignment: .leading, spacing: 2) {
                if let node = viewModel.selectedNode {
                    Text("Node \(node.nodeId)")
                        .font(.headline)
                    Text("\(node.speedDescription), \(Int(node.portCount)) ports")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                } else {
                    Text("ROM Details")
                        .font(.headline)
                    Text("Select a node from the left")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
            }

            Spacer()

            Button {
                viewModel.loadROMFromSelectedNodeCache()
            } label: {
                Label("Load Cache", systemImage: "tray.and.arrow.down")
            }
            .disabled(!viewModel.canReadSelectedNode || viewModel.isLoading)

            Button {
                viewModel.triggerROMReadForSelectedNode()
            } label: {
                Label("Read ROM", systemImage: "arrow.down.circle")
            }
            .buttonStyle(.borderedProminent)
            .disabled(!viewModel.canReadSelectedNode || viewModel.isLoading)
        }
        .padding(12)
        .background(Color(nsColor: .controlBackgroundColor))
    }

    @ViewBuilder
    private var statusArea: some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack(spacing: 12) {
                Label(viewModel.sourceDescription, systemImage: "externaldrive")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                if let gen = viewModel.topologyGeneration {
                    Label("Gen \(gen)", systemImage: "number")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
            }

            if let status = viewModel.statusMessage {
                Label(status, systemImage: viewModel.isLoading ? "hourglass" : "info.circle")
                    .font(.callout)
                    .foregroundStyle(.secondary)
            }

            if let error = viewModel.error {
                Label(error, systemImage: "exclamationmark.triangle.fill")
                    .font(.callout)
                    .foregroundStyle(.orange)
            }
        }
        .padding(12)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(Color(nsColor: .controlBackgroundColor).opacity(0.55))
    }

    private func startAutoRefresh() {
        stopAutoRefresh()
        autoRefreshTimer = Timer.scheduledTimer(withTimeInterval: 2.0, repeats: true) { _ in
            Task { @MainActor in
                viewModel.refreshAvailableNodes()
                if viewModel.selectedNode != nil {
                    viewModel.loadROMFromSelectedNodeCache()
                }
            }
        }
    }

    private func stopAutoRefresh() {
        autoRefreshTimer?.invalidate()
        autoRefreshTimer = nil
    }
}

private struct ROMNodeRow: View {
    let node: TopologyNode
    let isSelected: Bool
    let hasCachedROM: Bool

    var body: some View {
        HStack(spacing: 8) {
            Circle()
                .fill(node.linkActive ? Color.green : Color.gray)
                .frame(width: 8, height: 8)

            VStack(alignment: .leading, spacing: 3) {
                HStack(spacing: 6) {
                    Text("Node \(node.nodeId)")
                        .font(.headline)
                    if node.isRoot {
                        Image(systemName: "crown.fill")
                            .foregroundStyle(.orange)
                            .help("Root node")
                    }
                }

                Text("S\(node.maxSpeedMbps) • \(node.portCount) ports")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }

            Spacer()

            if hasCachedROM {
                Image(systemName: "doc.text.magnifyingglass")
                    .foregroundStyle(isSelected ? .primary : .secondary)
                    .help("ROM currently displayed")
            }
        }
        .padding(.vertical, 3)
    }
}

private struct ROMExplorerTabsView: View {
    let rom: RomTree
    let summary: RomSummary?
    let showInterpreted: Bool
    @Binding var showAdvancedBIB: Bool
    @Binding var treeSelectionID: String?

    var body: some View {
        TabView {
            ROMSummaryTab(rom: rom, summary: summary)
                .tabItem { Label("Summary", systemImage: "list.bullet.rectangle") }

            ROMBIBTab(rom: rom, showAdvanced: $showAdvancedBIB)
                .tabItem { Label("BIB", systemImage: "tablecells") }

            ROMTreeTab(rom: rom,
                       entries: showInterpreted ? RomInterpreter.interpretRoot(rom.rootDirectory) : rom.rootDirectory,
                       selectionID: $treeSelectionID)
                .tabItem { Label("Tree", systemImage: "list.bullet.indent") }

            ROMRawTab(data: rom.rawROM)
                .tabItem { Label("Raw", systemImage: "doc.plaintext") }
        }
        .padding(.top, 4)
    }
}

private struct ROMSummaryTab: View {
    let rom: RomTree
    let summary: RomSummary?

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 16) {
                GroupBox("ROM Overview") {
                    Grid(alignment: .leading, horizontalSpacing: 14, verticalSpacing: 8) {
                        GridRow {
                            Text("ROM size")
                                .foregroundStyle(.secondary)
                            Text("\(rom.rawROM.count) bytes (\(rom.rawROM.count / 4) quadlets)")
                        }
                        GridRow {
                            Text("Root dir start")
                                .foregroundStyle(.secondary)
                            Text("q\(rom.rootDirectoryStartQ) (byte \(rom.rootDirectoryStartQ * 4))")
                        }
                        GridRow {
                            Text("GUID")
                                .foregroundStyle(.secondary)
                            Text(String(format: "0x%016llX", rom.busInfo.guid))
                                .textSelection(.enabled)
                        }
                        GridRow {
                            Text("Vendor/chip")
                                .foregroundStyle(.secondary)
                            Text(String(format: "vendor=0x%06X chip=0x%010llX", rom.busInfo.nodeVendorID, rom.busInfo.chipID))
                        }
                    }
                    .padding(.vertical, 4)
                }

                if let summary {
                    GroupBox("Friendly Summary") {
                        VStack(alignment: .leading, spacing: 8) {
                            summaryRow("Vendor", value: friendlyVendor(summary))
                            summaryRow("Model", value: friendlyModel(summary))
                            if let modalias = summary.modalias {
                                summaryRow("Modalias", value: modalias, monospaced: true)
                            }
                            summaryRow("Unit directories", value: "\(summary.units.count)")
                        }
                        .padding(.vertical, 4)
                    }

                    if !summary.units.isEmpty {
                        GroupBox("Units") {
                            VStack(alignment: .leading, spacing: 8) {
                                ForEach(Array(summary.units.enumerated()), id: \.offset) { index, unit in
                                    VStack(alignment: .leading, spacing: 4) {
                                        Text("Unit \(index)")
                                            .font(.headline)
                                        if let spec = unit.specifierId {
                                            Text(String(format: "Specifier ID: 0x%06X%@", spec, spec == 0x00A02D ? " (AV/C)" : ""))
                                        }
                                        if let ver = unit.version {
                                            Text(String(format: "Version: 0x%06X", ver))
                                        }
                                        if let modelId = unit.modelId {
                                            Text(String(format: "Model ID: 0x%06X", modelId))
                                        }
                                        if let name = unit.modelName, !name.isEmpty {
                                            Text("Model Name: \(name)")
                                        }
                                    }
                                    .frame(maxWidth: .infinity, alignment: .leading)
                                    .padding(10)
                                    .background(Color(nsColor: .textBackgroundColor).opacity(0.4))
                                    .clipShape(RoundedRectangle(cornerRadius: 8))
                                }
                            }
                        }
                    }
                }

                GroupBox("Parser Diagnostics") {
                    if rom.diagnostics.isEmpty {
                        Text("No parser warnings. Unsupported descriptor encodings may still appear as raw leaf bytes.")
                            .foregroundStyle(.secondary)
                    } else {
                        VStack(alignment: .leading, spacing: 8) {
                            ForEach(rom.diagnostics) { diag in
                                Label(diag.message,
                                      systemImage: diag.severity == .warning ? "exclamationmark.triangle.fill" : "info.circle")
                                    .foregroundStyle(diag.severity == .warning ? .orange : .secondary)
                                    .font(.callout)
                            }
                        }
                    }
                }
            }
            .padding(14)
        }
    }

    private func summaryRow(_ label: String, value: String, monospaced: Bool = false) -> some View {
        HStack(alignment: .firstTextBaseline) {
            Text(label)
                .foregroundStyle(.secondary)
                .frame(width: 120, alignment: .leading)
            if monospaced {
                Text(value)
                    .font(.system(.body, design: .monospaced))
                    .textSelection(.enabled)
            } else {
                Text(value)
            }
            Spacer(minLength: 0)
        }
    }

    private func friendlyVendor(_ summary: RomSummary) -> String {
        if let name = summary.vendorName, !name.isEmpty {
            if let id = summary.vendorId {
                return String(format: "%@ (0x%06X)", name, id)
            }
            return name
        }
        if let id = summary.vendorId {
            return String(format: "0x%06X", id)
        }
        return "Unknown"
    }

    private func friendlyModel(_ summary: RomSummary) -> String {
        if let name = summary.modelName, !name.isEmpty {
            if let id = summary.modelId {
                return String(format: "%@ (0x%06X)", name, id)
            }
            return name
        }
        if let id = summary.modelId {
            return String(format: "0x%06X", id)
        }
        return "Unknown"
    }
}

private struct ROMBIBTab: View {
    let rom: RomTree
    @Binding var showAdvanced: Bool

    private var rows: [BIBFieldRow] {
        BIBFieldRow.makeRows(from: rom.busInfo)
    }

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 14) {
                HStack {
                    Text("Bus Information Block")
                        .font(.title3.weight(.semibold))
                    Spacer()
                    Toggle("Advanced", isOn: $showAdvanced)
                        .toggleStyle(.switch)
                        .help("Show raw quadlets and bit positions")
                }

                Text("This is the device's self-description header for IEEE 1394. The app decodes the bitfields and explains what they mean so you don't need to remember the masks.")
                    .font(.callout)
                    .foregroundStyle(.secondary)

                GroupBox("Decoded Fields") {
                    Grid(alignment: .leading, horizontalSpacing: 12, verticalSpacing: 8) {
                        GridRow {
                            Text("Field").font(.headline)
                            if showAdvanced { Text("Bits").font(.headline) }
                            Text("Value").font(.headline)
                            Text("Meaning").font(.headline)
                        }
                        Divider()
                        ForEach(rows) { row in
                            GridRow(alignment: .top) {
                                Text(row.name)
                                    .font(.system(.body, design: .monospaced))
                                if showAdvanced {
                                    Text(row.bits ?? "")
                                        .font(.system(.caption, design: .monospaced))
                                        .foregroundStyle(.secondary)
                                }
                                Text(row.value)
                                    .font(row.monospacedValue ? .system(.body, design: .monospaced) : .body)
                                    .textSelection(.enabled)
                                Text(row.description)
                                    .foregroundStyle(.secondary)
                                    .fixedSize(horizontal: false, vertical: true)
                            }
                            Divider()
                        }
                    }
                    .padding(.vertical, 4)
                }

                if showAdvanced {
                    GroupBox("Raw BIB Quadlets") {
                        VStack(alignment: .leading, spacing: 6) {
                            let h = rom.busInfo.header.rawQuadlet
                            Text(String(format: "q0 header     : 0x%08X", h)).font(.system(.body, design: .monospaced))
                            Text(String(format: "q1 bus_name   : 0x%08X ('%@')", rom.busInfo.busName, rom.busInfo.busNameString))
                                .font(.system(.body, design: .monospaced))
                            Text(String(format: "q2 busOptions : 0x%08X", rom.busInfo.busOptions.raw))
                                .font(.system(.body, design: .monospaced))
                            Text(String(format: "q3 guid_hi    : 0x%08X", rom.busInfo.guidHigh)).font(.system(.body, design: .monospaced))
                            Text(String(format: "q4 guid_lo    : 0x%08X", rom.busInfo.guidLow)).font(.system(.body, design: .monospaced))
                        }
                        .textSelection(.enabled)
                    }
                }
            }
            .padding(14)
        }
    }
}

private struct BIBFieldRow: Identifiable {
    let id = UUID()
    let name: String
    let bits: String?
    let value: String
    let description: String
    let monospacedValue: Bool

    static func makeRows(from bib: BusInfo) -> [BIBFieldRow] {
        let o = bib.busOptions
        return [
            .init(name: "bus_info_length", bits: "q0[31:24]", value: "\(bib.header.busInfoLength)", description: "Number of BIB quadlets after the header (q1..qN). The root directory starts immediately after these quadlets.", monospacedValue: false),
            .init(name: "crc_length", bits: "q0[23:16]", value: "\(bib.header.crcLength)", description: "How many quadlets are covered by the BIB CRC. This is CRC coverage, not total ROM size.", monospacedValue: false),
            .init(name: "crc", bits: "q0[15:0]", value: String(format: "0x%04X", bib.header.crc), description: "16-bit CRC for the BIB header coverage region. Useful for diagnostics; a mismatch should not scare users during normal browsing.", monospacedValue: true),
            .init(name: "bus_name", bits: "q1", value: String(format: "0x%08X ('%@')", bib.busName, bib.busNameString), description: "Bus identifier. Most IEEE 1394 devices report ASCII '1394'.", monospacedValue: true),
            .init(name: "irmc", bits: "q2[31]", value: o.irmc ? "Yes" : "No", description: "Isochronous Resource Manager capable bit.", monospacedValue: false),
            .init(name: "cmc", bits: "q2[30]", value: o.cmc ? "Yes" : "No", description: "Cycle master capable bit.", monospacedValue: false),
            .init(name: "isc", bits: "q2[29]", value: o.isc ? "Yes" : "No", description: "Isochronous capable bit.", monospacedValue: false),
            .init(name: "bmc", bits: "q2[28]", value: o.bmc ? "Yes" : "No", description: "Bus manager capable bit.", monospacedValue: false),
            .init(name: "pmc", bits: "q2[27]", value: o.pmc ? "Yes" : "No", description: "Power manager capable bit (if implemented by the device).", monospacedValue: false),
            .init(name: "cyc_clk_acc", bits: "q2[23:16]", value: String(format: "0x%02X (%d)", o.cycClkAcc, o.cycClkAcc), description: "Cycle clock accuracy code. This is a timing-quality hint used for synchronization diagnostics, not something the user typically needs to tweak.", monospacedValue: true),
            .init(name: "max_rec", bits: "q2[15:12]", value: "\(o.maxRec) (≈ \(maxAsyncPayloadBytes(maxRec: o.maxRec)) bytes max async payload)", description: "Maximum asynchronous receive payload code. Higher values allow larger packets.", monospacedValue: false),
            .init(name: "max_ROM", bits: "q2[9:8]", value: "\(o.maxRom)", description: "Config ROM read capability encoding. Many devices still work fine with quadlet reads only.", monospacedValue: false),
            .init(name: "generation", bits: "q2[7:4]", value: "\(o.generation)", description: "Device's BIB generation nibble. This can differ from the host's current topology generation seen elsewhere in the app.", monospacedValue: false),
            .init(name: "link_spd", bits: "q2[2:0]", value: "\(o.linkSpd) (\(linkSpeedLabel(code: o.linkSpd)))", description: "Maximum link speed code advertised in the BIB bus-options quadlet.", monospacedValue: false),
            .init(name: "guid", bits: "q3:q4", value: String(format: "0x%016llX", bib.guid), description: "Globally unique identifier for the node. This is usually the most stable identifier for a device across resets.", monospacedValue: true)
        ]
    }

    private static func maxAsyncPayloadBytes(maxRec: UInt8) -> Int {
        1 << Int(maxRec + 1)
    }

    private static func linkSpeedLabel(code: UInt8) -> String {
        switch code {
        case 0: return "S100"
        case 1: return "S200"
        case 2: return "S400"
        case 3: return "S800"
        case 4: return "S1600"
        case 5: return "S3200"
        default: return "Reserved/Unknown"
        }
    }
}

private struct ROMTreeTab: View {
    let rom: RomTree
    let entries: [DirectoryEntry]
    @Binding var selectionID: String?

    private var nodes: [ROMOutlineNode] {
        entries.enumerated().map { index, entry in
            ROMOutlineNode.from(entry, fallbackLabel: "entry \(index)")
        }
    }

    private var selectedNode: ROMOutlineNode? {
        guard let selectionID else { return nil }
        return nodes.firstMatch(id: selectionID)
    }

    var body: some View {
        HSplitView {
            List(selection: $selectionID) {
                Section {
                    OutlineGroup(nodes, children: \.children) { node in
                        ROMTreeNodeRow(node: node)
                            .tag(node.id)
                    }
                } header: {
                    Text("Root Directory (q\(rom.rootDirectoryStartQ))")
                }
            }
            .frame(minWidth: 320)

            ScrollView {
                VStack(alignment: .leading, spacing: 12) {
                    if let node = selectedNode {
                        GroupBox("Entry Details") {
                            VStack(alignment: .leading, spacing: 8) {
                                Text(node.title)
                                    .font(.headline)
                                if let subtitle = node.subtitle {
                                    Text(subtitle)
                                        .foregroundStyle(.secondary)
                                }
                                if let q = node.entry.entryQuadletIndex {
                                    Text("Entry quadlet: q\(q)")
                                }
                                if let rel = node.entry.relativeOffset24 {
                                    Text("Relative offset: \(rel) quadlets")
                                }
                                if let target = node.entry.targetQuadletIndex {
                                    Text("Target quadlet: q\(target)")
                                }
                                if let raw = node.entry.rawEntryWord {
                                    Text(String(format: "Raw entry: 0x%08X", raw))
                                        .font(.system(.body, design: .monospaced))
                                        .textSelection(.enabled)
                                }
                            }
                            .frame(maxWidth: .infinity, alignment: .leading)
                        }

                        GroupBox("Decoded Value") {
                            ROMValueDetailView(value: node.entry.value)
                                .frame(maxWidth: .infinity, alignment: .leading)
                        }
                    } else {
                        VStack(spacing: 10) {
                            Image(systemName: "cursorarrow.click")
                                .font(.title2)
                                .foregroundStyle(.secondary)
                            Text("Select an entry in the tree")
                                .foregroundStyle(.secondary)
                            Text("Leaves and nested directories are shown using IEEE 1212 key/type decoding.")
                                .font(.callout)
                                .foregroundStyle(.tertiary)
                                .multilineTextAlignment(.center)
                                .frame(maxWidth: 340)
                        }
                        .frame(maxWidth: .infinity, minHeight: 220)
                    }
                }
                .padding(14)
            }
            .frame(minWidth: 280)
        }
    }
}

private struct ROMOutlineNode: Identifiable, Hashable {
    let id: String
    let title: String
    let subtitle: String?
    let entry: DirectoryEntry
    let children: [ROMOutlineNode]?

    static func == (lhs: ROMOutlineNode, rhs: ROMOutlineNode) -> Bool {
        lhs.id == rhs.id
    }

    func hash(into hasher: inout Hasher) {
        hasher.combine(id)
    }

    static func from(_ entry: DirectoryEntry, fallbackLabel: String) -> ROMOutlineNode {
        let title = "\(entry.keyName) • \(entry.typeLabel)"
        let subtitle = entry.valueSummary
        let children: [ROMOutlineNode]?
        if case .directory(let subEntries) = entry.value {
            children = subEntries.enumerated().map { idx, child in
                ROMOutlineNode.from(child, fallbackLabel: "\(fallbackLabel).\(idx)")
            }
        } else {
            children = nil
        }
        return ROMOutlineNode(id: entry.id, title: title, subtitle: subtitle, entry: entry, children: children)
    }
}

private extension Array where Element == ROMOutlineNode {
    func firstMatch(id: String) -> ROMOutlineNode? {
        for node in self {
            if node.id == id { return node }
            if let child = node.children?.firstMatch(id: id) { return child }
        }
        return nil
    }
}

private struct ROMTreeNodeRow: View {
    let node: ROMOutlineNode

    var body: some View {
        VStack(alignment: .leading, spacing: 2) {
            HStack(spacing: 6) {
                Image(systemName: node.entry.type == .directory ? "folder" : "doc.text")
                    .foregroundStyle(node.entry.type == .directory ? .blue : .secondary)
                Text(node.entry.keyName)
                Text(node.entry.typeLabel)
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
            if let subtitle = node.subtitle, !subtitle.isEmpty {
                Text(subtitle)
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
            }
        }
        .padding(.vertical, 2)
    }
}

private struct ROMValueDetailView: View {
    let value: RomValue

    var body: some View {
        switch value {
        case .immediate(let v):
            detailRow("Immediate", String(format: "0x%06X (%u)", v, v), monospaced: true)
        case .csrOffset(let v):
            detailRow("CSR Offset", String(format: "0x%012llX", v), monospaced: true)
        case .leafPlaceholder(let offset):
            detailRow("Leaf", "Placeholder (target byte offset \(offset))")
        case .leafDescriptorText(let s, let bytes):
            VStack(alignment: .leading, spacing: 8) {
                detailRow("Text", s)
                if !bytes.isEmpty {
                    detailRow("Bytes", bytes.map { String(format: "%02X", $0) }.joined(separator: " "), monospaced: true)
                }
            }
        case .leafEUI64(let v):
            detailRow("EUI-64", String(format: "0x%016llX", v), monospaced: true)
        case .leafData(let d):
            VStack(alignment: .leading, spacing: 8) {
                detailRow("Leaf bytes", "\(d.count)")
                if !d.isEmpty {
                    Text(hexPreview(d))
                        .font(.system(.caption, design: .monospaced))
                        .textSelection(.enabled)
                        .padding(8)
                        .background(Color(nsColor: .textBackgroundColor).opacity(0.45))
                        .clipShape(RoundedRectangle(cornerRadius: 8))
                }
            }
        case .directory(let d):
            detailRow("Directory", "\(d.count) entries")
        }
    }

    private func detailRow(_ label: String, _ value: String, monospaced: Bool = false) -> some View {
        HStack(alignment: .firstTextBaseline) {
            Text(label)
                .foregroundStyle(.secondary)
                .frame(width: 90, alignment: .leading)
            if monospaced {
                Text(value)
                    .font(.system(.body, design: .monospaced))
                    .textSelection(.enabled)
            } else {
                Text(value)
            }
            Spacer(minLength: 0)
        }
    }

    private func hexPreview(_ data: Data, maxBytes: Int = 96) -> String {
        let slice = data.prefix(maxBytes)
        let hex = slice.map { String(format: "%02X", $0) }.joined(separator: " ")
        return data.count > maxBytes ? "\(hex) ..." : hex
    }
}

private struct ROMRawTab: View {
    let data: Data

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 16) {
                GroupBox("Summary") {
                    Grid(alignment: .leading, horizontalSpacing: 12, verticalSpacing: 8) {
                        GridRow {
                            Text("Size")
                                .foregroundStyle(.secondary)
                            Text("\(data.count) bytes (\(data.count / 4) quadlets)")
                        }
                    }
                    .padding(.vertical, 4)
                }

                GroupBox("Hex Dump (Big-endian wire order)") {
                    Text(hexDump())
                        .font(.system(.body, design: .monospaced))
                        .textSelection(.enabled)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .padding(8)
                }

                GroupBox("Quadlets") {
                    LazyVStack(alignment: .leading, spacing: 3) {
                        ForEach(0..<(data.count / 4), id: \.self) { index in
                            Text(quadletLine(index: index))
                                .font(.system(.body, design: .monospaced))
                                .textSelection(.enabled)
                                .frame(maxWidth: .infinity, alignment: .leading)
                        }
                    }
                    .padding(8)
                }
            }
            .padding(14)
        }
    }

    private func hexDump() -> String {
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

    private func quadletLine(index: Int) -> String {
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
}

private extension DirectoryEntry {
    var typeLabel: String {
        switch type {
        case .immediate: return "immediate"
        case .csrOffset: return "csr_offset"
        case .leaf: return "leaf"
        case .directory: return "directory"
        }
    }

    var valueSummary: String {
        switch value {
        case .immediate(let v):
            return String(format: "0x%06X", v)
        case .csrOffset(let v):
            return String(format: "0x%012llX", v)
        case .leafPlaceholder:
            return "leaf target out of bounds"
        case .leafDescriptorText(let text, _):
            return text.isEmpty ? "text descriptor (empty)" : "\"\(text)\""
        case .leafEUI64(let v):
            return String(format: "EUI-64 0x%016llX", v)
        case .leafData(let data):
            return "\(data.count) bytes"
        case .directory(let entries):
            return "\(entries.count) entries"
        }
    }
}

#if DEBUG
struct ROMExplorerView_Previews: PreviewProvider {
    static var previews: some View {
        ROMExplorerView(viewModel: RomExplorerViewModel())
            .frame(width: 1100, height: 760)
    }
}
#endif
