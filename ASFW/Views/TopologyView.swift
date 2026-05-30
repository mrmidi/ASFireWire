//
//  TopologyView.swift
//  ASFW
//
//  Created by ASFireWire Project on 07.10.2025.
//

import SwiftUI

struct TopologyView: View {
    @ObservedObject var viewModel: TopologyViewModel
    @State private var selectedNode: TopologyNode?
    @State private var showSelfIDDetail = false
    
    var body: some View {
        VStack(spacing: 0) {
            // Header with refresh controls
            HStack {
                Text("Topology & Self-ID")
                    .font(.title2)
                    .fontWeight(.semibold)
                
                Spacer()
                
                if viewModel.isLoading {
                    ProgressView()
                        .controlSize(.small)
                        .frame(width: 16, height: 16)
                }
                
                Button(action: { viewModel.refresh() }) {
                    Image(systemName: "arrow.clockwise")
                }
                .buttonStyle(.borderless)
            }
            .padding()
            .background(Color(NSColor.controlBackgroundColor))
            
            if let error = viewModel.error {
                ContentUnavailableView(
                    "No Topology Data",
                    systemImage: "network.slash",
                    description: Text(error)
                )
            } else if let topology = viewModel.topology {
                ScrollView {
                    VStack(alignment: .leading, spacing: 16) {
                        // Topology summary
                        topologySummaryCard(topology)
                        
                        // Self-ID section
                        if let selfID = viewModel.selfIDCapture {
                            selfIDCard(selfID)
                        }
                        
                        // Topology tree (rooted at the bus root)
                        topologyTreeCard(topology)

                        // Per-PHY reported port detail (collapsible)
                        portDetailCard(topology)

                        // Warnings
                        if !topology.warnings.isEmpty {
                            warningsCard(topology.warnings)
                        }
                    }
                    .padding()
                }
            } else {
                ContentUnavailableView(
                    "Waiting for Bus Reset",
                    systemImage: "antenna.radiowaves.left.and.right",
                    description: Text("Topology data will appear after the next bus reset")
                )
            }
        }
        .sheet(isPresented: $showSelfIDDetail) {
            if let selfID = viewModel.selfIDCapture {
                SelfIDDetailView(capture: selfID)
            }
        }
    }
    
    // MARK: - Summary Card
    
    private func topologySummaryCard(_ topology: TopologySnapshot) -> some View {
        GroupBox {
            VStack(alignment: .leading, spacing: 8) {
                HStack {
                    Text("Bus Generation")
                    Spacer()
                    Text("\(topology.generation)")
                        .fontWeight(.semibold)
                }
                
                HStack {
                    Text("Node Count")
                    Spacer()
                    Text("\(topology.nodeCount)")
                        .fontWeight(.semibold)
                }
                
                if let rootId = topology.rootNodeId {
                    HStack {
                        Text("Root Node")
                        Spacer()
                        Text("Node \(rootId)")
                            .fontWeight(.semibold)
                            .foregroundColor(.green)
                    }
                }
                
                if let irmId = topology.irmNodeId {
                    HStack {
                        Text("IRM Node")
                        Spacer()
                        Text("Node \(irmId)")
                            .fontWeight(.semibold)
                            .foregroundColor(.blue)
                    }
                }
                
                if let localId = topology.localNodeId {
                    HStack {
                        Text("Local Node")
                        Spacer()
                        Text("Node \(localId)")
                            .fontWeight(.semibold)
                            .foregroundColor(.orange)
                    }
                }
                
                HStack {
                    Text("Gap Count")
                    Spacer()
                    Text("\(topology.gapCount)")
                        .fontWeight(.semibold)
                }
            }
        } label: {
            Label("Topology Summary", systemImage: "network")
                .font(.headline)
        }
    }
    
    // MARK: - Self-ID Card
    
    private func selfIDCard(_ selfID: SelfIDCapture) -> some View {
        GroupBox {
            VStack(alignment: .leading, spacing: 8) {
                HStack {
                    VStack(alignment: .leading, spacing: 4) {
                        Text("Generation: \(selfID.generation)")
                            .font(.caption)
                        Text("Quadlets: \(selfID.rawQuadlets.count)")
                            .font(.caption)
                        Text("Packets: \(selfID.packets.count)")
                            .font(.caption)
                    }
                    
                    Spacer()
                    
                    VStack(alignment: .trailing, spacing: 4) {
                        HStack(spacing: 4) {
                            if selfID.valid {
                                Image(systemName: "checkmark.circle.fill")
                                    .foregroundColor(.green)
                                Text("Valid")
                                    .font(.caption)
                            } else {
                                Image(systemName: "xmark.circle.fill")
                                    .foregroundColor(.red)
                                Text("Invalid")
                                    .font(.caption)
                            }
                        }
                        
                        if selfID.crcError {
                            Text("CRC Error")
                                .font(.caption)
                                .foregroundColor(.red)
                        }
                        
                        if selfID.timedOut {
                            Text("Timeout")
                                .font(.caption)
                                .foregroundColor(.orange)
                        }
                    }
                }
                
                if let error = selfID.errorReason {
                    Text("Error: \(error)")
                        .font(.caption)
                        .foregroundColor(.red)
                }
                
                Button("View Self-ID Details...") {
                    showSelfIDDetail = true
                }
                .buttonStyle(.link)

                if !selfID.rawQuadlets.isEmpty {
                    Divider()
                        .padding(.vertical, 4)

                    LazyVStack(alignment: .leading, spacing: 4) {
                        Text("Raw Quadlets")
                            .font(.caption)
                            .foregroundColor(.secondary)

                        ForEach(Array(selfID.rawQuadlets.enumerated()), id: \.0) { index, quad in
                            Text(String(format: "[%02d] 0x%08X", index, quad))
                                .font(.system(.caption, design: .monospaced))
                                .foregroundColor(.primary)
                        }
                    }
                }
            }
        } label: {
            Label("Self-ID Capture", systemImage: "doc.text.magnifyingglass")
                .font(.headline)
        }
    }
    
    // MARK: - Topology Tree

    /// parent node id -> child nodes, derived from each node's parent port link.
    private func childrenByParent(_ topology: TopologySnapshot) -> [UInt8: [TopologyNode]] {
        var map: [UInt8: [TopologyNode]] = [:]
        for node in topology.nodes where !node.isRoot {
            guard let parentPort = node.parentPort, Int(parentPort) < node.links.count else { continue }
            let link = node.links[Int(parentPort)]
            guard link.connected else { continue }
            map[link.remoteNodeId, default: []].append(node)
        }
        for key in map.keys { map[key]?.sort { $0.nodeId < $1.nodeId } }
        return map
    }

    private func topologyTreeCard(_ topology: TopologySnapshot) -> some View {
        let children = childrenByParent(topology)
        // Root(s): nodes flagged isRoot; fall back to the highest id if none.
        let roots = topology.nodes.filter { $0.isRoot }
        let effectiveRoots = roots.isEmpty ? Array(topology.nodes.suffix(1)) : roots

        return GroupBox {
            if topology.nodes.isEmpty {
                Text("No nodes")
                    .font(.caption)
                    .foregroundColor(.secondary)
                    .frame(maxWidth: .infinity, alignment: .leading)
            } else {
                VStack(alignment: .leading, spacing: 6) {
                    ForEach(effectiveRoots) { root in
                        TopologyTreeNodeView(
                            node: root,
                            parentLinkSpeed: nil,
                            topology: topology,
                            children: children,
                            selectedNode: $selectedNode
                        )
                    }
                }
                .frame(maxWidth: .infinity, alignment: .leading)
            }
        } label: {
            Label("Bus Topology Tree", systemImage: "circle.hexagongrid.fill")
                .font(.headline)
        }
    }

    // MARK: - Per-PHY Port Detail (collapsible)

    private func portDetailCard(_ topology: TopologySnapshot) -> some View {
        GroupBox {
            DisclosureGroup("Per-PHY reported ports") {
                LazyVStack(alignment: .leading, spacing: 12) {
                    ForEach(topology.nodes) { node in
                        nodeRow(node, topology: topology)
                            .background(selectedNode?.nodeId == node.nodeId ? Color.accentColor.opacity(0.1) : Color.clear)
                            .cornerRadius(4)
                            .onTapGesture { selectedNode = node }
                    }
                }
                .padding(.top, 4)
            }
            .font(.subheadline)
        } label: {
            Label("Self-ID Port Detail", systemImage: "rectangle.split.3x1")
                .font(.headline)
        }
    }

    private func nodeRow(_ node: TopologyNode, topology: TopologySnapshot) -> some View {
        VStack(alignment: .leading, spacing: 4) {
            HStack {
                // Node ID badge
                Text("Node \(node.nodeId)")
                    .font(.system(.body, design: .monospaced))
                    .fontWeight(.semibold)
                    .padding(.horizontal, 8)
                    .padding(.vertical, 2)
                    .background(nodeColor(node, topology: topology))
                    .foregroundColor(.white)
                    .cornerRadius(4)
                
                // Speed
                Text(node.speedDescription)
                    .font(.caption)
                    .foregroundColor(.secondary)
                
                // Flags
                if node.isRoot {
                    Image(systemName: "crown.fill")
                        .foregroundColor(.yellow)
                }
                
                if node.isIRMCandidate {
                    Image(systemName: "star.fill")
                        .foregroundColor(.blue)
                }
                
                if node.initiatedReset {
                    Image(systemName: "bolt.fill")
                        .foregroundColor(.orange)
                }
                
                Spacer()
                
                // Port count
                Text("\(node.portStates.count) ports")
                    .font(.caption)
                    .foregroundColor(.secondary)
            }
            
            // Port states visualization
            HStack(spacing: 4) {
                ForEach(Array(node.portStates.enumerated()), id: \.offset) { index, state in
                    VStack(spacing: 2) {
                        Text(state.icon)
                            .font(.caption2)
                        Text("P\(index)")
                            .font(.system(size: 8))
                            .foregroundColor(.secondary)
                    }
                    .help(state.description)
                }
            }
            .padding(.leading, 8)
        }
        .padding(8)
    }
    
    private func nodeColor(_ node: TopologyNode, topology: TopologySnapshot) -> Color {
        if topology.localNodeId == node.nodeId {
            return .orange
        } else if topology.rootNodeId == node.nodeId {
            return .green
        } else if topology.irmNodeId == node.nodeId {
            return .blue
        } else {
            return .gray
        }
    }
    
    // MARK: - Warnings Card
    
    private func warningsCard(_ warnings: [String]) -> some View {
        GroupBox {
            VStack(alignment: .leading, spacing: 8) {
                ForEach(warnings, id: \.self) { warning in
                    HStack(alignment: .top, spacing: 8) {
                        Image(systemName: "exclamationmark.triangle.fill")
                            .foregroundColor(.orange)
                        Text(warning)
                            .font(.caption)
                    }
                }
            }
        } label: {
            Label("Topology Warnings", systemImage: "exclamationmark.triangle")
                .font(.headline)
                .foregroundColor(.orange)
        }
    }
}

// MARK: - Recursive Tree Node

/// One node in the bus topology tree, drawn rooted at the bus root. Children are
/// the nodes whose parent-port link points back at this node; the recursion walks
/// down the tree via the physical adjacency carried in TopologyNode.links.
private struct TopologyTreeNodeView: View {
    let node: TopologyNode
    /// Mbps of the edge from this node's parent, nil for the root.
    let parentLinkSpeed: UInt32?
    let topology: TopologySnapshot
    let children: [UInt8: [TopologyNode]]
    @Binding var selectedNode: TopologyNode?

    private var childList: [TopologyNode] { children[node.nodeId] ?? [] }

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            nodeBadge

            if !childList.isEmpty {
                VStack(alignment: .leading, spacing: 6) {
                    ForEach(childList) { child in
                        HStack(alignment: .top, spacing: 6) {
                            Text("└─")
                                .font(.system(.body, design: .monospaced))
                                .foregroundColor(.secondary)
                            TopologyTreeNodeView(
                                node: child,
                                parentLinkSpeed: edgeSpeed(node, child),
                                topology: topology,
                                children: children,
                                selectedNode: $selectedNode
                            )
                        }
                    }
                }
                .padding(.leading, 14)
            }
        }
    }

    private var nodeBadge: some View {
        HStack(spacing: 8) {
            Text("Node \(node.nodeId)")
                .font(.system(.body, design: .monospaced))
                .fontWeight(.semibold)
                .padding(.horizontal, 8)
                .padding(.vertical, 2)
                .background(accentColor)
                .foregroundColor(.white)
                .cornerRadius(4)

            Text(node.speedDescription)
                .font(.caption)
                .foregroundColor(.secondary)

            if let edge = parentLinkSpeed {
                Text("· \(edge) Mbps link")
                    .font(.caption2)
                    .foregroundColor(.secondary)
            }

            if node.isRoot {
                Image(systemName: "crown.fill").foregroundColor(.yellow).help("Bus root")
            }
            if topology.irmNodeId == node.nodeId {
                Image(systemName: "star.fill").foregroundColor(.blue).help("Isochronous Resource Manager")
            }
            if topology.localNodeId == node.nodeId {
                Image(systemName: "person.fill").foregroundColor(.orange).help("Local node")
            }
            if node.initiatedReset {
                Image(systemName: "bolt.fill").foregroundColor(.orange).help("Initiated last bus reset")
            }
            if !node.linkActive {
                Text("PHY-only")
                    .font(.caption2)
                    .foregroundColor(.secondary)
                    .help("Link layer inactive (repeater)")
            }
        }
        .padding(.vertical, 2)
        .background(selectedNode?.nodeId == node.nodeId ? Color.accentColor.opacity(0.12) : Color.clear)
        .cornerRadius(4)
        .contentShape(Rectangle())
        .onTapGesture { selectedNode = node }
    }

    private var accentColor: Color {
        if topology.localNodeId == node.nodeId { return .orange }
        if topology.rootNodeId == node.nodeId { return .green }
        if topology.irmNodeId == node.nodeId { return .blue }
        return .gray
    }

    /// A cable runs at the slower of the two PHYs' max speeds.
    private func edgeSpeed(_ a: TopologyNode, _ b: TopologyNode) -> UInt32 {
        let sa = a.maxSpeedMbps == 0 ? b.maxSpeedMbps : a.maxSpeedMbps
        let sb = b.maxSpeedMbps == 0 ? a.maxSpeedMbps : b.maxSpeedMbps
        return min(sa, sb)
    }
}
