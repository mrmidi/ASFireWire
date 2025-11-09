//
//  ROMExplorerView.swift
//  ASFW
//
//  Config ROM Explorer with on-demand reading capability
//

import SwiftUI

struct ROMExplorerView: View {
    @ObservedObject var viewModel: DebugViewModel
    @State private var selectedNodeId: UInt8?
    @State private var romData: Data?
    @State private var statusMessage: String?
    @State private var isLoading = false
    @State private var autoRefreshEnabled = false
    @State private var autoRefreshTimer: Timer?

    var body: some View {
        HSplitView {
            // Left sidebar: Node list
            nodeListView
                .frame(minWidth: 250, idealWidth: 300)

            // Right detail: ROM display
            romDetailView
                .frame(minWidth: 400)
        }
        .navigationTitle("ROM Explorer")
        .toolbar {
            ToolbarItem(placement: .automatic) {
                Toggle("Auto-refresh", isOn: $autoRefreshEnabled)
                    .toggleStyle(.switch)
                    .onChange(of: autoRefreshEnabled) { _, newValue in
                        if newValue {
                            startAutoRefresh()
                        } else {
                            stopAutoRefresh()
                        }
                    }
            }
        }
        .onDisappear {
            stopAutoRefresh()
        }
    }

    private var nodeListView: some View {
        VStack(alignment: .leading, spacing: 0) {
            // Header
            HStack {
                Text("Nodes")
                    .font(.headline)
                    .padding()
                Spacer()
                Button {
                    refreshTopology()
                } label: {
                    Label("Refresh", systemImage: "arrow.clockwise")
                }
                .buttonStyle(.borderless)
                .padding(.trailing)
            }
            .background(Color(NSColor.controlBackgroundColor))

            Divider()

            // Connection status
                if !viewModel.isConnected {
                VStack(spacing: 12) {
                    Image(systemName: "cable.connector.slash")
                        .font(.largeTitle)
                        .foregroundStyle(.secondary)
                    Text("Driver not connected")
                        .foregroundStyle(.secondary)
                }
                .boundedPlaceholder(minHeight: 120)
            } else if let topology = viewModel.topologyCache {
                // Node list
                List(topology.nodes, id: \.nodeId, selection: $selectedNodeId) { node in
                    NodeRowView(node: node)
                        .tag(node.nodeId)
                }
                .listStyle(.sidebar)
                .onChange(of: selectedNodeId) { _, newNodeId in
                    if let nodeId = newNodeId {
                        loadROMForNode(nodeId)
                    } else {
                        romData = nil
                    }
                }
                } else {
                VStack(spacing: 12) {
                    ProgressView()
                    Text("Loading topology...")
                        .foregroundStyle(.secondary)
                }
                .boundedPlaceholder(minHeight: 120)
            }
        }
    }

    private var romDetailView: some View {
        VStack(alignment: .leading, spacing: 0) {
            // Header
            HStack {
                if let nodeId = selectedNodeId {
                    Text("Node \(nodeId)")
                        .font(.headline)
                } else {
                    Text("No node selected")
                        .font(.headline)
                        .foregroundStyle(.secondary)
                }
                Spacer()

                if let nodeId = selectedNodeId {
                    Button {
                        triggerROMRead(nodeId: nodeId)
                    } label: {
                        Label("Read ROM", systemImage: "arrow.down.circle")
                    }
                    .buttonStyle(.borderedProminent)
                    .disabled(isLoading || !viewModel.isConnected)
                }
            }
            .padding()
            .background(Color(NSColor.controlBackgroundColor))

            Divider()

            // Status message
            if let message = statusMessage {
                HStack {
                    Image(systemName: isLoading ? "hourglass" : "info.circle")
                    Text(message)
                        .font(.callout)
                    Spacer()
                }
                .padding()
                .background(Color(NSColor.controlBackgroundColor).opacity(0.5))
            }

            // ROM content
            ScrollView {
                if isLoading {
                    VStack(spacing: 16) {
                        ProgressView()
                        Text("Reading ROM...")
                            .foregroundStyle(.secondary)
                    }
                    .boundedPlaceholder(minHeight: 120)
                    .padding()
                } else if let data = romData, !data.isEmpty {
                    ROMDataView(data: data)
                        .padding()
                } else if selectedNodeId != nil {
                    VStack(spacing: 16) {
                        Image(systemName: "memorychip.fill")
                            .font(.system(size: 48))
                            .foregroundStyle(.secondary)
                        Text("ROM not cached")
                            .font(.title3)
                            .foregroundStyle(.secondary)
                        Text("Click 'Read ROM' to fetch the Config ROM for this node")
                            .font(.callout)
                            .foregroundStyle(.tertiary)
                            .multilineTextAlignment(.center)
                    }
                    .boundedPlaceholder(minHeight: 120)
                    .padding()
                } else {
                    VStack(spacing: 16) {
                        Image(systemName: "sidebar.left")
                            .font(.system(size: 48))
                            .foregroundStyle(.secondary)
                        Text("Select a node from the list")
                            .foregroundStyle(.secondary)
                    }
                    .boundedPlaceholder(minHeight: 120)
                }
            }
        }
    }

    private func refreshTopology() {
        viewModel.fetchTopology()
        if let nodeId = selectedNodeId {
            loadROMForNode(nodeId)
        }
    }

    private func loadROMForNode(_ nodeId: UInt8) {
        guard let topology = viewModel.topologyCache else { return }

        statusMessage = "Checking ROM cache..."

        // Try to get ROM from cache
        if let data = viewModel.connector.getConfigROM(nodeId: nodeId, generation: UInt16(topology.generation)) {
            romData = data
            statusMessage = "ROM loaded from cache (\(data.count) bytes)"
        } else {
            romData = nil
            statusMessage = "ROM not cached for this node"
        }
    }

    private func triggerROMRead(nodeId: UInt8) {
        print("[ROMExplorerView] 🔵 triggerROMRead called for nodeId=\(nodeId) (0x\(String(nodeId, radix: 16)))")
        print("[ROMExplorerView]    isConnected=\(viewModel.isConnected)")
        print("[ROMExplorerView]    topology generation=\(viewModel.topologyCache?.generation ?? 0)")

        isLoading = true
        statusMessage = "Initiating ROM read..."

        let status = viewModel.connector.triggerROMRead(nodeId: nodeId)

        print("[ROMExplorerView] 🔵 triggerROMRead returned status: \(status)")

        switch status {
        case .initiated:
            statusMessage = "ROM read initiated, waiting for completion..."
            print("[ROMExplorerView] ✅ ROM read initiated, starting polling...")
            // Poll for completion
            DispatchQueue.main.asyncAfter(deadline: .now() + 1.0) {
                checkROMReadComplete(nodeId: nodeId, retries: 10)
            }
        case .alreadyInProgress:
            statusMessage = "ROM read already in progress..."
            print("[ROMExplorerView] ⚠️ ROM read already in progress")
            isLoading = false
        case .failed:
            // Include detailed error from connector
            let detailedError = viewModel.connector.lastError ?? "Unknown error"
            statusMessage = "Failed to initiate ROM read: \(detailedError)"
            print("[ROMExplorerView] ❌ ROM read failed: \(detailedError)")
            isLoading = false
        }
    }

    private func checkROMReadComplete(nodeId: UInt8, retries: Int) {
        guard retries > 0, let topology = viewModel.topologyCache else {
            isLoading = false
            if retries == 0 {
                statusMessage = "ROM read timeout - check driver logs"
            }
            return
        }

        // Try to fetch ROM
        if let data = viewModel.connector.getConfigROM(nodeId: nodeId, generation: UInt16(topology.generation)) {
            romData = data
            statusMessage = "ROM read complete! (\(data.count) bytes)"
            isLoading = false
        } else {
            // Still not available, retry
            statusMessage = "Waiting for ROM read... (retry \(11 - retries)/10)"
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) {
                checkROMReadComplete(nodeId: nodeId, retries: retries - 1)
            }
        }
    }

    private func startAutoRefresh() {
        autoRefreshTimer = Timer.scheduledTimer(withTimeInterval: 2.0, repeats: true) { _ in
            if let nodeId = selectedNodeId {
                loadROMForNode(nodeId)
            }
        }
    }

    private func stopAutoRefresh() {
        autoRefreshTimer?.invalidate()
        autoRefreshTimer = nil
    }
}

struct NodeRowView: View {
    let node: TopologyNode

    var body: some View {
        HStack {
            Image(systemName: "circle.fill")
                .font(.system(size: 8))
                .foregroundStyle(node.linkActive ? .green : .gray)

            VStack(alignment: .leading, spacing: 4) {
                Text("Node \(node.nodeId)")
                    .font(.headline)

                HStack(spacing: 12) {
                    Label("\(node.portCount) ports", systemImage: "point.3.connected.trianglepath.dotted")
                        .font(.caption)
                    Label("S\(node.maxSpeedMbps)", systemImage: "speedometer")
                        .font(.caption)
                }
                .foregroundStyle(.secondary)
            }

            Spacer()

            if node.isRoot {
                Image(systemName: "crown.fill")
                    .foregroundStyle(.orange)
                    .help("Root node")
            }
        }
        .padding(.vertical, 4)
    }
}

struct ROMDataView: View {
    let data: Data

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            // Summary
            GroupBox("Summary") {
                Grid(alignment: .leading, horizontalSpacing: 12, verticalSpacing: 8) {
                    GridRow {
                        Text("Size:")
                            .foregroundStyle(.secondary)
                        Text("\(data.count) bytes (\(data.count / 4) quadlets)")
                    }

                    if data.count >= 16 {
                        GridRow {
                            Text("Bus Info Block:")
                                .foregroundStyle(.secondary)
                            Text("Present")
                                .foregroundStyle(.green)
                        }
                    }
                }
                .padding()
            }

            // Hex dump
            GroupBox("Raw Data (Host Byte Order)") {
                ScrollView {
                    Text(hexDump())
                        .font(.system(.body, design: .monospaced))
                        .textSelection(.enabled)
                        .padding()
                }
                .frame(maxHeight: 400)
            }

            // Quadlet view
            if data.count >= 4 {
                GroupBox("Quadlets") {
                    ScrollView {
                        LazyVStack(alignment: .leading, spacing: 2) {
                            ForEach(0..<(data.count / 4), id: \.self) { index in
                                quadletRow(index: index)
                            }
                        }
                        .padding()
                    }
                    .frame(maxHeight: 300)
                }
            }
        }
    }

    private func hexDump() -> String {
        var result = ""
        let bytesPerLine = 16

        for offset in stride(from: 0, to: data.count, by: bytesPerLine) {
            // Offset
            result += String(format: "%08x: ", offset)

            // Hex bytes
            for i in 0..<bytesPerLine {
                let byteOffset = offset + i
                if byteOffset < data.count {
                    result += String(format: "%02x ", data[byteOffset])
                } else {
                    result += "   "
                }

                if i == 7 {
                    result += " "
                }
            }

            result += " "

            // ASCII representation
            for i in 0..<bytesPerLine {
                let byteOffset = offset + i
                if byteOffset < data.count {
                    let byte = data[byteOffset]
                    if byte >= 32 && byte < 127 {
                        result += String(format: "%c", byte)
                    } else {
                        result += "."
                    }
                }
            }

            result += "\n"
        }

        return result
    }

    private func quadletRow(index: Int) -> some View {
        let offset = index * 4
        guard offset + 4 <= data.count else {
            return AnyView(EmptyView())
        }

        // Read as big-endian (IEEE 1394 wire format)
        let quadlet = data.withUnsafeBytes { ptr in
            let bytes = ptr.bindMemory(to: UInt8.self)
            return UInt32(bytes[offset]) << 24 |
                   UInt32(bytes[offset + 1]) << 16 |
                   UInt32(bytes[offset + 2]) << 8 |
                   UInt32(bytes[offset + 3])
        }

        return AnyView(
            HStack {
                Text(String(format: "Q%-3d", index))
                    .foregroundStyle(.secondary)
                    .frame(width: 50, alignment: .leading)
                Text(String(format: "0x%08x", quadlet))
                    .font(.system(.body, design: .monospaced))
            }
            .padding(.vertical, 2)
        )
    }
}

#if DEBUG
struct ROMExplorerView_Previews: PreviewProvider {
    static var previews: some View {
        ROMExplorerView(viewModel: DebugViewModel())
            .frame(width: 900, height: 600)
    }
}
#endif
