import SwiftUI

/// One node selection shared by discovery, Config ROM, and bus topology.
struct BusInspectorView: View {
    @ObservedObject var debugViewModel: DebugViewModel
    @ObservedObject var topologyViewModel: TopologyViewModel
    @ObservedObject var romViewModel: RomExplorerViewModel
    @State private var selectedID: String?
    @State private var topologyNodeID: UInt8?
    @State private var tab: BusInspectorTab = .device

    private var nodes: [BusInspectorNode] {
        guard debugViewModel.isConnected else { return [] }
        return BusInspectorNode.inventory(topology: topologyViewModel.topology,
                                          devices: debugViewModel.discoveredDevices)
    }

    private var selected: BusInspectorNode? { nodes.first { $0.id == selectedID } }

    // Reload the selected node's cache after a reset or node renumbering, too.
    private var selectionContext: String {
        guard let selected else { return "none" }
        return "\(selected.id):\(selected.generation):\(selected.node.nodeId)"
    }

    var body: some View {
        VStack(spacing: 0) {
            HStack {
                Label("Bus Inspector", systemImage: "network")
                    .font(.title2.bold())
                Spacer()
                if debugViewModel.isConnected, let topology = topologyViewModel.topology {
                    Text("Generation \(topology.generation) · \(topology.nodeCount) nodes")
                        .font(.callout)
                        .foregroundStyle(.secondary)
                }
                Button("Refresh", systemImage: "arrow.clockwise", action: refresh)
                    .disabled(!debugViewModel.isConnected)
            }
            .padding()
            Divider()

            HSplitView {
                List(nodes, selection: $selectedID) { item in
                    BusInspectorNodeRow(item: item)
                        .tag(item.id)
                }
                .listStyle(.inset)
                .frame(minWidth: 240, idealWidth: 280, maxWidth: 340, maxHeight: .infinity)

                VStack(alignment: .leading, spacing: 0) {
                    if let selected {
                        VStack(alignment: .leading, spacing: 6) {
                            Text(selected.title).font(.title2.bold())
                            Text("Node \(selected.node.nodeId) · S\(selected.node.maxSpeedMbps) · \(selected.node.portCount) ports")
                                .font(.callout)
                                .foregroundStyle(.secondary)
                            if let device = selected.device { StateLabel(state: device.stateString) }
                        }
                        .padding()
                    }
                    Picker("Inspect", selection: $tab) {
                        ForEach(BusInspectorTab.allCases) { tab in
                            Text(tab.rawValue).tag(tab)
                        }
                    }
                    .pickerStyle(.segmented)
                    .padding(.horizontal)
                    .padding(.vertical, 10)
                    Divider()
                    Group {
                        if !debugViewModel.isConnected {
                            ContentUnavailableView("Driver Not Connected", systemImage: "cable.connector.slash",
                                                   description: Text("Install the driver and connect a FireWire controller to inspect the bus."))
                        } else if tab == .topology {
                            TopologyView(viewModel: topologyViewModel, selectedNodeID: $topologyNodeID)
                        } else if let selected {
                            switch tab {
                            case .device:
                                if let device = selected.device {
                                    DeviceDetailView(device: device)
                                } else {
                                    ContentUnavailableView(selected.isLocal ? "Local Controller" : "Discovery Pending",
                                                           systemImage: "cpu",
                                                           description: Text("Inspect this node's Config ROM or its topology and Self-ID data."))
                                }
                            case .rom:
                                ROMExplorerView(viewModel: romViewModel)
                            case .topology:
                                EmptyView()
                            }
                        } else {
                            ContentUnavailableView("No Bus Nodes", systemImage: "network",
                                                   description: Text("Nodes appear when the driver publishes a topology snapshot."))
                        }
                    }
                    .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
                }
                .frame(minWidth: 460, maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
        .navigationTitle("Bus Inspector")
        .onAppear(perform: reconcileSelection)
        .onChange(of: nodes.map(\.id)) { _, _ in reconcileSelection() }
        .onChange(of: selectionContext) { _, _ in synchronizeSelection() }
        .onChange(of: topologyNodeID) { _, nodeID in
            if let item = nodes.first(where: { $0.node.nodeId == nodeID }) { selectedID = item.id }
        }
    }

    private func refresh() {
        debugViewModel.manualRefresh()
        topologyViewModel.refresh()
    }

    private func reconcileSelection() {
        if !nodes.contains(where: { $0.id == selectedID }) {
            selectedID = nodes.first(where: { !$0.isLocal })?.id ?? nodes.first?.id
        }
        synchronizeSelection()
    }

    private func synchronizeSelection() {
        topologyNodeID = selected?.node.nodeId
        romViewModel.selectNode(selected?.node)
        if selected != nil { romViewModel.loadROMFromSelectedNodeCache() }
    }
}
