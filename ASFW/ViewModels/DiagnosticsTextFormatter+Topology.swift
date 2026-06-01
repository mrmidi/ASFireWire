//
//  DiagnosticsTextFormatter+Topology.swift
//  ASFW
//
//  Topology & Self-ID section: per-node decode table, the bus tree, and the raw
//  Self-ID quadlets. This section materializes the largest temporaries in the
//  whole report (the ~17 KB `topology.nodes` array via withUnsafeBytes), so
//  isolating it in its own stack frame is the main reason the report is split.
//

import Foundation

extension DiagnosticsTextFormatter {

    static func appendTopology(_ r: DiagnosticsReport,
                               _ snapshot: ASFWDiagnosticsSnapshot) {
        let pad = DiagnosticsReport.pad

        r.title("Topology & Self-ID")
        r.row("Topology Valid", snapshot.topology.valid != 0 ? "Yes" : "No")
        r.row("Self-ID Sequence Count", snapshot.topology.selfIdSequenceCount)
        r.row("Enumerator Error", snapshot.topology.enumeratorError != 0 ? "Yes (Error Code: \(snapshot.topology.enumeratorError))" : "No")

        let count = Int(clamping: snapshot.topology.nodeCount)
        r.row("Topology Node Count", count)

        // Extract nodes and rawSelfIds array from tuples
        let nodes: [ASFWDiagNode] = withUnsafeBytes(of: snapshot.topology.nodes) { buffer in
            let bound = buffer.bindMemory(to: ASFWDiagNode.self)
            return Array(bound)
        }
        let rawSelfIds: [UInt32] = withUnsafeBytes(of: snapshot.topology.rawSelfIds) { buffer in
            let bound = buffer.bindMemory(to: UInt32.self)
            return Array(bound)
        }

        if snapshot.topology.valid != 0 && count > 0 {
            r.raw("\nDecoded Node Details:\n")
            r.raw("  " + pad("NodeID", 8) + " " + pad("Local", 7) + " " + pad("Root", 7) + " "
                + pad("Contender", 10) + " " + pad("Speed", 5) + " " + pad("Power", 11) + " "
                + pad("LinkActive", 10) + " " + pad("Ports", 8) + "\n")
            r.raw("  " + String(repeating: "─", count: 73) + "\n")

            for i in 0..<min(count, nodes.count) {
                let node = nodes[i]
                var portDetails = ""
                let pCount = Int(min(node.portCount, UInt32(ASFW_DIAG_MAX_PORTS)))

                let portStates: [UInt32] = withUnsafeBytes(of: node.ports) { buffer in
                    let bound = buffer.bindMemory(to: UInt32.self)
                    return Array(bound)
                }

                for p in 0..<pCount {
                    let portState = portStates[p]
                    let stateChar: String
                    switch portState {
                    case ASFWDiagPortStateNotPresent.rawValue: stateChar = "." // not present
                    case ASFWDiagPortStateNotConnected.rawValue: stateChar = "-" // present, not connected
                    case ASFWDiagPortStateParent.rawValue: stateChar = "P"   // toward root
                    case ASFWDiagPortStateChild.rawValue: stateChar = "C"    // away from root
                    default: stateChar = "?"
                    }
                    portDetails += stateChar
                }

                let speedStr = DiagFormat.speed4(node.speed)

                let portDetailsPad = pad(portDetails, 8)
                r.raw("  " + pad(String(node.nodeId), 8) + " "
                    + pad(node.isLocal != 0 ? "Yes" : "No", 7) + " "
                    + pad(node.isRoot != 0 ? "Yes" : "No", 7) + " "
                    + pad(node.contender != 0 ? "Yes" : "No", 10) + " "
                    + pad(speedStr, 5) + " "
                    + pad(DiagFormat.powerClass(node.powerClass), 11) + " "
                    + pad(node.linkActive != 0 ? "Yes" : "No", 10) + " "
                    + portDetailsPad + "\n")
            }

            // Topology tree, rooted at the bus root, built from parentPort + links[]
            // (same adjacency the GUI tree uses).
            let validNodes = Array(nodes.prefix(min(count, nodes.count)))
            let byId = Dictionary(validNodes.map { (Int($0.nodeId), $0) }, uniquingKeysWith: { a, _ in a })

            let speedLabel = DiagFormat.speedExt
            func mbps(_ s: UInt32) -> Int { Int(speedLabel(s).dropFirst()) ?? 0 }
            func parentOf(_ n: ASFWDiagNode) -> Int? {
                guard n.isRoot == 0, n.parentPort != 0xFFFF_FFFF else { return nil }
                let links: [UInt32] = withUnsafeBytes(of: n.links) { Array($0.bindMemory(to: UInt32.self)) }
                let pp = Int(n.parentPort)
                guard pp < links.count, links[pp] != 0xFFFF_FFFF else { return nil }
                return Int((links[pp] >> 8) & 0xFF)
            }

            var childrenOf: [Int: [Int]] = [:]
            for node in validNodes {
                if let parent = parentOf(node) { childrenOf[parent, default: []].append(Int(node.nodeId)) }
            }
            for key in childrenOf.keys { childrenOf[key]?.sort() }

            let rootList = validNodes.filter { $0.isRoot != 0 }.map { Int($0.nodeId) }
            let effectiveRoots = rootList.isEmpty ? (validNodes.last.map { [Int($0.nodeId)] } ?? []) : rootList

            if !effectiveRoots.isEmpty {
                r.raw("\nTopology Tree (rooted at bus root):\n")
                // Hoist the only two snapshot fields the recursion needs into locals.
                // Capturing the whole multi-KB `snapshot` value type in the nested
                // recursive function bloats every stack frame; on the small-stacked
                // dispatch worker this formatter runs on, that overflows the stack.
                let irmNodeId = Int(snapshot.topology.irmNode)
                let localNodeId = Int(snapshot.topology.localNode)
                // Topology links come from a driver snapshot and are not guaranteed
                // to form an acyclic tree (stale/malformed parentPort -> back-edge).
                // Guard against cycles so a bad snapshot degrades gracefully instead
                // of overflowing the stack via unbounded recursion.
                //
                // TODO (worth considering, not urgent): converting this recursion to
                // an explicit work-stack iteration would eliminate stack depth as a
                // failure mode entirely. This formatter runs on a small-stacked
                // libdispatch worker (~512 KB), so deep/degenerate topologies are the
                // only remaining theoretical risk now that the per-frame footprint
                // and cycles are handled. Recursion is fine for real bus sizes today.
                var visited: Set<Int> = []
                func renderTreeNode(_ id: Int, prefix: String, isLast: Bool, isRootRow: Bool) {
                    guard let node = byId[id] else { return }
                    guard visited.insert(id).inserted else {
                        let connector = isRootRow ? "" : (isLast ? "└─ " : "├─ ")
                        r.raw("  \(prefix)\(connector)Node \(id)  [cycle]\n")
                        return
                    }
                    let connector = isRootRow ? "" : (isLast ? "└─ " : "├─ ")
                    var tags: [String] = []
                    if node.isRoot != 0 { tags.append("root") }
                    if irmNodeId == id { tags.append("IRM") }
                    if localNodeId == id { tags.append("local") }
                    if node.initiatedReset != 0 { tags.append("reset-init") }
                    if node.linkActive == 0 { tags.append("PHY-only") }
                    var edge = ""
                    if let parent = parentOf(node), let pnode = byId[parent] {
                        let a = mbps(node.speed) == 0 ? mbps(pnode.speed) : mbps(node.speed)
                        let b = mbps(pnode.speed) == 0 ? mbps(node.speed) : mbps(pnode.speed)
                        let e = min(a, b)
                        if e > 0 { edge = " (\(e) Mbps link)" }
                    }
                    let tagStr = tags.isEmpty ? "" : "  [" + tags.joined(separator: ",") + "]"
                    r.raw("  \(prefix)\(connector)Node \(id)  \(speedLabel(node.speed))\(edge)\(tagStr)\n")
                    let kids = childrenOf[id] ?? []
                    let childPrefix = prefix + (isRootRow ? "" : (isLast ? "   " : "│  "))
                    for (idx, child) in kids.enumerated() {
                        renderTreeNode(child, prefix: childPrefix, isLast: idx == kids.count - 1, isRootRow: false)
                    }
                }
                for (idx, root) in effectiveRoots.enumerated() {
                    renderTreeNode(root, prefix: "", isLast: idx == effectiveRoots.count - 1, isRootRow: true)
                }
            }
        }

        let selfIdCount = Int(min(snapshot.topology.rawSelfIdCount, UInt32(ASFW_DIAG_MAX_SELF_ID_QUADS)))
        if selfIdCount > 0 {
            r.raw("\nRaw Self-ID Quadlets (\(selfIdCount)):\n")
            for i in 0..<selfIdCount {
                r.raw(String(format: "  [%02d]: 0x%08X\n", i, rawSelfIds[i]))
            }
        }
    }
}
