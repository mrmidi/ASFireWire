//
//  DiagnosticsTextFormatter.swift
//  ASFW
//
//  Created by ASFireWire Project on 29.05.2026.
//

import Foundation

struct DiagnosticsTextFormatter {
    
    static func format(snapshot: ASFWDiagnosticsSnapshot) -> String {
        var report = ""
        
        func appendTitle(_ title: String) {
            report += "\n"
            report += "=== \(title) ===\n"
            report += String(repeating: "─", count: title.count + 8) + "\n"
        }
        
        // NOTE: Swift's String(format:) does NOT support %s safely — it expects a C char*,
        // but Swift String/[CChar] bridge to objects, so %s runs strlen on an object pointer
        // and crashes (EXC_BAD_ACCESS). All column alignment is done with Swift padding instead.
        func pad(_ s: String, _ width: Int) -> String {
            s.count >= width ? s : s.padding(toLength: width, withPad: " ", startingAt: 0)
        }

        func appendRow(_ label: String, _ value: Any) {
            report += pad(label + ":", 30) + " " + String(describing: value) + "\n"
        }
        
        // Valid bus node IDs are 0..62; 0x3F (63), 0xFF and 0xFFFFFFFF are "no node"/sentinels.
        func formatNodeStr(_ nodeVal: UInt32) -> String {
            if nodeVal >= 0x3F {
                return "none"
            }
            return String(format: "node %d / 0x%04X", nodeVal, 0xFFC0 + nodeVal)
        }

        // RoleAction::Kind (ASFWDriver/Bus/Role/RolePolicy.hpp).
        func roleActionName(_ v: UInt32) -> String {
            switch v {
            case 0: return "None (stable)"
            case 1: return "DeferForEvidence"
            case 2: return "EnableLocalCycleMaster"
            case 3: return "EnableRemoteCycleMaster"
            case 4: return "ForceRootAndReset"
            case 5: return "ClearContenderAndDelegate"
            case 6: return "MarkRootBadOrUnknown"
            default: return "Unknown (\(v))"
            }
        }

        // RoleResetFlavor (None/Short/Long).
        func roleResetName(_ v: UInt32) -> String {
            switch v {
            case 0: return "None"
            case 1: return "Short"
            case 2: return "Long"
            default: return "Unknown (\(v))"
            }
        }

        // pwr field per IEEE 1394-2008 self-ID packet 0.
        func powerClassName(_ pc: UInt32) -> String {
            switch pc {
            case 0: return "none"
            case 1: return "self+15W"
            case 2: return "self+30W"
            case 3: return "self+45W"
            case 4: return "bus≤3W"
            case 5: return "reserved"
            case 6: return "bus+3Wlink"
            case 7: return "bus+7Wlink"
            default: return "\(pc)"
            }
        }

        func formatUptime(_ ns: UInt64) -> String {
            let totalSec = ns / 1_000_000_000
            let h = totalSec / 3600
            let m = (totalSec % 3600) / 60
            let s = totalSec % 60
            return String(format: "%dh %02dm %02ds", h, m, s)
        }
        
        func formatHex16(_ val: UInt32) -> String {
            return String(format: "0x%04X", val)
        }
        
        func formatHex32(_ val: UInt32) -> String {
            return String(format: "0x%08X", val)
        }
        
        func formatHex64(_ val: UInt64) -> String {
            return String(format: "0x%016llX", val)
        }
        
        // --- Report Header ---
        report += "ASFW 1394 DIAGNOSTICS REPORT\n"
        report += String(repeating: "═", count: 60) + "\n"
        // The driver header timestamp is mach uptime (since boot), NOT wall-clock. The real
        // wall-clock time is added app-side at report-generation time.
        appendRow("Report Generated", Date().description)
        appendRow("Driver Uptime", formatUptime(snapshot.busContract.header.timestampNs))
        appendRow("Driver Uptime (ns)", snapshot.busContract.header.timestampNs)
        appendRow("ABI Version", snapshot.busContract.header.abiVersion)
        appendRow("Generation", snapshot.busContract.header.generation)
        appendRow("Snapshot Sequence", snapshot.busContract.header.snapshotSeq)
        
        // --- Bus Contract ---
        appendTitle("Bus Contract")
        appendRow("Bus ID", snapshot.busContract.busId)
        appendRow("Local Node", formatNodeStr(snapshot.busContract.localNode))
        appendRow("Root Node", formatNodeStr(snapshot.busContract.rootNode))
        appendRow("IRM Node", formatNodeStr(snapshot.busContract.irmNode))
        appendRow("BM Node", formatNodeStr(snapshot.busContract.bmNode))
        appendRow("Node Count", snapshot.busContract.nodeCount)
        appendRow("Gap Count", snapshot.busContract.gapCount)
        // maxHops==0 is only valid for a single-node bus; on a multi-node bus it means the
        // parent/child adjacency wasn't built when the snapshot was taken (not computed).
        if snapshot.busContract.maxHops == 0 && snapshot.busContract.nodeCount > 1 {
            appendRow("Max Hops", "unavailable (not computed)")
        } else {
            appendRow("Max Hops", snapshot.busContract.maxHops)
        }
        appendRow("Cycle Start Observed", snapshot.busContract.cycleStartObserved != 0 ? "Yes" : "No")
        appendRow("Cycle Start Source Node", formatNodeStr(snapshot.busContract.cycleStartSourceNode))
        appendRow("Local Cycle Master Enabled", snapshot.busContract.localCycleMasterEnabled != 0 ? "Yes" : "No")
        appendRow("Local Cycle Timer Enabled", snapshot.busContract.localCycleTimerEnabled != 0 ? "Yes" : "No")
        appendRow("Last Bus Reset Count", snapshot.busContract.lastBusResetCount)
        
        let policyStr: String
        switch snapshot.busContract.rolePolicyMode {
        case 0: policyStr = "Standard"
        case 1: policyStr = "Force Root"
        case 2: policyStr = "Force Not Root"
        default: policyStr = "Unknown (\(snapshot.busContract.rolePolicyMode))"
        }
        appendRow("Role Policy Mode", policyStr)
        
        appendRow("Role Verdict (last action)", roleActionName(snapshot.busContract.roleVerdict))
        
        // --- Topology & Self-ID ---
        appendTitle("Topology & Self-ID")
        appendRow("Topology Valid", snapshot.topology.valid != 0 ? "Yes" : "No")
        appendRow("Self-ID Sequence Count", snapshot.topology.selfIdSequenceCount)
        appendRow("Enumerator Error", snapshot.topology.enumeratorError != 0 ? "Yes (Error Code: \(snapshot.topology.enumeratorError))" : "No")
        
        let count = Int(clamping: snapshot.topology.nodeCount)
        appendRow("Topology Node Count", count)
        
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
            report += "\nDecoded Node Details:\n"
            report += "  " + pad("NodeID", 8) + " " + pad("Local", 7) + " " + pad("Root", 7) + " "
                + pad("Contender", 10) + " " + pad("Speed", 5) + " " + pad("Power", 11) + " "
                + pad("LinkActive", 10) + " " + pad("Ports", 8) + "\n"
            report += "  " + String(repeating: "─", count: 73) + "\n"
            
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
                    case ASFWDiagPortStateInactive.rawValue: stateChar = "I" // Inactive
                    case ASFWDiagPortStateChild.rawValue: stateChar = "C"    // Child
                    case ASFWDiagPortStateParent.rawValue: stateChar = "P"   // Parent
                    case ASFWDiagPortStateUnknown.rawValue: stateChar = "U"  // Unknown
                    default: stateChar = "?"
                    }
                    portDetails += stateChar
                }
                
                let speedStr: String
                switch node.speed {
                case ASFWDiagSpeedS100.rawValue: speedStr = "S100"
                case ASFWDiagSpeedS200.rawValue: speedStr = "S200"
                case ASFWDiagSpeedS400.rawValue: speedStr = "S400"
                case ASFWDiagSpeedS800.rawValue: speedStr = "S800"
                default: speedStr = "S?"
                }
                
                let portDetailsPad = pad(portDetails, 8)
                report += "  " + pad(String(node.nodeId), 8) + " "
                    + pad(node.isLocal != 0 ? "Yes" : "No", 7) + " "
                    + pad(node.isRoot != 0 ? "Yes" : "No", 7) + " "
                    + pad(node.contender != 0 ? "Yes" : "No", 10) + " "
                    + pad(speedStr, 5) + " "
                    + pad(powerClassName(node.powerClass), 11) + " "
                    + pad(node.linkActive != 0 ? "Yes" : "No", 10) + " "
                    + portDetailsPad + "\n"
            }
        }
        
        let selfIdCount = Int(min(snapshot.topology.rawSelfIdCount, UInt32(ASFW_DIAG_MAX_SELF_ID_QUADS)))
        if selfIdCount > 0 {
            report += "\nRaw Self-ID Quadlets (\(selfIdCount)):\n"
            for i in 0..<selfIdCount {
                report += String(format: "  [%02d]: 0x%08X\n", i, rawSelfIds[i])
            }
        }
        
        // --- Role Coordinator ---
        appendTitle("Role Coordinator Policy Engine")
        let rcModeStr: String
        switch snapshot.roleCoordinator.policyMode {
        case 0: rcModeStr = "Standard"
        case 1: rcModeStr = "Force Root"
        case 2: rcModeStr = "Force Not Root"
        default: rcModeStr = "Unknown (\(snapshot.roleCoordinator.policyMode))"
        }
        appendRow("Policy Mode", rcModeStr)
        appendRow("Last Decision", roleActionName(snapshot.roleCoordinator.lastDecision))
        appendRow("Last Action", roleActionName(snapshot.roleCoordinator.lastAction))
        appendRow("Last Reset Flavor", roleResetName(snapshot.roleCoordinator.lastActionResult))
        // These two are sourced from the ROOT node's BIB cycle-master evidence, NOT the local
        // node's enable state — relabel to avoid the contradiction with the authoritative
        // hardware bit (shown from the Bus Contract / OHCI LinkControl).
        appendRow("Root CMC Known (evidence)", snapshot.roleCoordinator.localCycleMasterAllowed != 0 ? "Yes" : "No")
        appendRow("Root CMC Capable (evidence)", snapshot.roleCoordinator.localCycleMasterEnabled != 0 ? "Yes" : "No")
        appendRow("Local Cycle Master Enabled (HW)", snapshot.busContract.localCycleMasterEnabled != 0 ? "Yes" : "No")

        // The CMSTR fields are only meaningful when a remote-cycle-master write was actually
        // issued. When unset (all zero) render "none" rather than a misleading node 0 / addr 0.
        let cmstrExecuted = snapshot.roleCoordinator.remoteCMSTRAddress != 0
            || snapshot.roleCoordinator.remoteCMSTRResult != 0
            || snapshot.roleCoordinator.remoteCMSTRPayload != 0
        if cmstrExecuted {
            appendRow("Remote CMSTR Target Node", formatNodeStr(snapshot.roleCoordinator.remoteCMSTRTargetNode))
            appendRow("Remote CMSTR Result", formatHex32(snapshot.roleCoordinator.remoteCMSTRResult))
            appendRow("Remote CMSTR Address", formatHex64(snapshot.roleCoordinator.remoteCMSTRAddress))
            appendRow("Remote CMSTR Payload", formatHex32(snapshot.roleCoordinator.remoteCMSTRPayload))
            appendRow("Remote CMSTR RCode", snapshot.roleCoordinator.remoteCMSTRRCode)
        } else {
            appendRow("Remote CMSTR Policy", "observe-only / not armed")
            appendRow("Reason", "root CMC evidence not connected")
        }
        appendRow("Cycle Start Observed", snapshot.roleCoordinator.cycleStartObserved != 0 ? "Yes" : "No")
        appendRow("Cycle Start Source Node", formatNodeStr(snapshot.roleCoordinator.cycleStartSourceNode))
        appendRow("Reset Guard Active", snapshot.roleCoordinator.resetGuardActive != 0 ? "Yes" : "No")
        appendRow("BM Retry Count", snapshot.roleCoordinator.bmRetryCount)
        appendRow("Gap Mismatch Detected", snapshot.roleCoordinator.gapMismatchDetected != 0 ? "Yes" : "No")
        
        // --- Bus Manager ---
        appendTitle("Bus Manager & IRM Role Runtime")
        let bmModeStr: String
        switch snapshot.busManager.roleMode {
        case 0: bmModeStr = "Legacy BMC Cleared"
        case 1: bmModeStr = "Apple Avoid Manager"
        case 2: bmModeStr = "IRM Server Only"
        case 3: bmModeStr = "Full Bus Manager"
        default: bmModeStr = "Unknown (\(snapshot.busManager.roleMode))"
        }
        appendRow("Configured Role Mode", bmModeStr)
        appendRow("Advertised BMC", snapshot.busManager.advertisedBmc != 0 ? "Yes" : "No")
        appendRow("Advertised IRMC", snapshot.busManager.advertisedIrmc != 0 ? "Yes" : "No")
        appendRow("Advertised CMC", snapshot.busManager.advertisedCmc != 0 ? "Yes" : "No")
        appendRow("Advertised ISC", snapshot.busManager.advertisedIsc != 0 ? "Yes" : "No")
        
        appendRow("Local Node is IRM", snapshot.busManager.localIsIRM != 0 ? "Yes" : "No")
        appendRow("Local Node is BM", snapshot.busManager.localIsBM != 0 ? "Yes" : "No")
        appendRow("Local Node is Root", snapshot.busManager.localIsRoot != 0 ? "Yes" : "No")
        
        let bmOwnerStr: String
        switch snapshot.busManager.bmOwnerSource {
        case 0: bmOwnerStr = "Unknown"
        case 1: bmOwnerStr = "Inferred"
        case 2: bmOwnerStr = "BusManagerIdRead"
        case 3: bmOwnerStr = "ElectionResult"
        case 4: bmOwnerStr = "LocalWonElection"
        case 5: bmOwnerStr = "RemoteWonElection"
        default: bmOwnerStr = "Unknown (\(snapshot.busManager.bmOwnerSource))"
        }
        appendRow("BM Owner Source", bmOwnerStr)
        appendRow("Last BM ID Value", formatNodeStr(snapshot.busManager.lastBusManagerIdOldValue))
        appendRow("Stale Election Aborts", snapshot.busManager.staleElectionAbortCount)
        appendRow("Failed Elections", snapshot.busManager.failedElectionCount)
        appendRow("Unexpected software resource accesses", snapshot.busManager.unexpectedResourceCsrSoftwareCount)
        
        appendRow("Local IRM BM ID Reg", formatNodeStr(snapshot.busManager.localIrmBusManagerId))
        appendRow("Local IRM Bandwidth Available", snapshot.busManager.localIrmBandwidthAvailable)
        appendRow("Local IRM Channels Avail Hi", formatHex32(snapshot.busManager.localIrmChannelsAvailableHi))
        appendRow("Local IRM Channels Avail Lo", formatHex32(snapshot.busManager.localIrmChannelsAvailableLo))
        
        appendRow("Topology Map Valid", snapshot.busManager.topologyMapValid != 0 ? "Yes" : "No")
        appendRow("Topology Map Generation", snapshot.busManager.topologyMapGeneration)
        appendRow("Topology Map Self-ID Count", snapshot.busManager.topologyMapSelfIdCount)
        appendRow("Topology Map CRC", formatHex16(snapshot.busManager.topologyMapCRC))
        appendRow("Topology Map DMA Ready", snapshot.busManager.topologyMapDMAReady != 0 ? "Yes" : "No")

        // --- OHCI Registers ---
        appendTitle("OHCI Link/Controller Snapshot")
        appendRow("OHCI Version", formatHex32(snapshot.ohci.version))
        appendRow("GUID ROM Present", snapshot.ohci.guidROM != 0 ? "Yes" : "No")
        appendRow("AT Tx Retries", snapshot.ohci.atRetries)
        appendRow("CSR Control Register", formatHex32(snapshot.ohci.csrControl))
        appendRow("Config ROM Header Reg", formatHex32(snapshot.ohci.configROMHeader))
        appendRow("Bus Options Register", formatHex32(snapshot.ohci.busOptions))
        appendRow("Node ID Register", formatHex32(snapshot.ohci.nodeId))
        appendRow("PHY Control Register", formatHex32(snapshot.ohci.phyControl))
        appendRow("Cycle Timer Register", formatHex32(snapshot.ohci.isochronousCycleTimer))
        appendRow("GUID (64-bit Hex)", String(format: "0x%08X%08X", snapshot.ohci.guidHi, snapshot.ohci.guidLo))
        appendRow("HC Control Register (Set)", formatHex32(snapshot.ohci.hcControlSet))
        appendRow("Link Control Register (Set)", formatHex32(snapshot.ohci.linkControlSet))
        appendRow("Interrupt Event (Active)", formatHex32(snapshot.ohci.intEventSet))
        appendRow("Interrupt Mask (Enabled)", formatHex32(snapshot.ohci.intMaskSet))
        appendRow("Self-ID Buffer Pointer", formatHex32(snapshot.ohci.selfIdBuffer))
        appendRow("Self-ID Count Register", formatHex32(snapshot.ohci.selfIdCount))
        
        // --- PHY Status ---
        appendTitle("PHY Interface Snapshot")
        appendRow("Link On", snapshot.phy.linkOn != 0 ? "Yes" : "No")
        appendRow("Contender", snapshot.phy.contender != 0 ? "Yes" : "No")
        appendRow("PHY Gap Count", snapshot.phy.gapCount)
        appendRow("Last PhyConfig Root ID", formatNodeStr(snapshot.phy.lastPhyConfigRootId))
        appendRow("Last PhyConfig Gap Count", snapshot.phy.lastPhyConfigGapCount)
        appendRow("Last PHY Reset Reason", snapshot.phy.lastPhyResetReason)
        
        let regs: [UInt32] = withUnsafeBytes(of: snapshot.phy.regs) { buffer in
            let bound = buffer.bindMemory(to: UInt32.self)
            return Array(bound)
        }
        let regCount = Int(min(snapshot.phy.regCount, UInt32(ASFW_DIAG_MAX_PHY_REGS)))
        if regCount > 0 {
            report += "\nPHY Registers (raw):\n"
            for r in 0..<regCount {
                report += String(format: "  Reg %02d: 0x%02X\n", r, regs[r])
            }
        }

        // --- PHY Interpretation (decode + consistency live in PhyDiagnostics, below) ---
        let phy = PhyDecode(regs: regs, validMask: snapshot.phy.regValidMask)
        appendTitle("PHY Interpretation")
        // regValidMask tells us which reads actually succeeded (OHCI rdDone). With it we can
        // distinguish a genuine 0xFF (isolated PHY: physical_id=63) from a failed/timed-out read.
        if !phy.reg0Valid {
            // Benign: reg 0 is redundant. The in-tree Linux reference never reads PHY reg 0 —
            // node identity comes from OHCI NodeID. Source it from there instead of alarming.
            let ohciNode = snapshot.ohci.nodeId & 0x3F
            let ohciRoot = (snapshot.ohci.nodeId >> 30) & 1 == 1
            appendRow("Reg00", "not read (rdDone not set) — reg 0 is redundant; identity from OHCI NodeID")
            appendRow("Reg00 physical_id", "\(ohciNode) (from OHCI NodeID)")
            appendRow("Reg00 root", "\(ohciRoot ? "Yes" : "No") (from OHCI NodeID)")
            appendRow("Reg00 power_status", "n/a (reg 0 not read)")
        } else {
            appendRow("Reg00 physical_id", phy.physicalId == 63 ? "63 / unassigned (isolated)" : "\(phy.physicalId)")
            appendRow("Reg00 root", phy.root ? "Yes" : "No")
            appendRow("Reg00 power_status", phy.powerStatus ? "Yes" : "No")
        }
        appendRow("Reg01 root_holdoff", phy.rootHoldoff ? "Yes" : "No")
        appendRow("Reg01 initiate_reset", phy.initiateBusReset ? "Yes" : "No")
        appendRow("Reg01 gap_count", phy.gapCount)
        appendRow("Reg02 extended", phy.extended == 7 ? "Yes (0b111)" : "\(phy.extended)")
        appendRow("Reg02 total_ports", phy.totalPorts)
        appendRow("Reg03 max_speed_field", phy.maxSpeedField)
        appendRow("Reg03 repeater_delay", "\(phy.repeaterDelay) (~\(144 + Int(phy.repeaterDelay) * 20) ns)")
        appendRow("Reg04 link_on", phy.linkOn ? "Yes" : "No")
        appendRow("Reg04 contender", phy.contender ? "Yes" : "No")
        appendRow("Reg04 jitter", phy.jitter)
        appendRow("Reg04 power_class", "\(phy.powerClass) / \(powerClassName(phy.powerClass))")
        appendRow("Reg05 pwr_fail", phy.powerFail ? "Yes (may be latched, verify)" : "No")
        appendRow("Reg05 loop", phy.loop ? "Yes" : "No")
        appendRow("Reg05 timeout", phy.timeout ? "Yes" : "No")
        appendRow("Reg05 port_event", phy.portEvent ? "Yes" : "No")
        appendRow("Reg06 page_select", phy.pageSelect)
        appendRow("Reg06 port_select", phy.portSelect)

        // --- PHY Consistency vs OHCI NodeID / Self-ID topology ---
        let cons = PhyConsistencyChecker.check(phy: phy, regs: regs,
                                               bus: snapshot.busContract,
                                               topo: snapshot.topology,
                                               ohci: snapshot.ohci)
        appendTitle("PHY Consistency")
        let ohciNodeValid = (snapshot.ohci.nodeId >> 31) & 1 == 1
        let ohciNode = snapshot.ohci.nodeId & 0x3F
        appendRow("OHCI NodeID (authoritative)", ohciNodeValid ? "valid, \(formatNodeStr(ohciNode))" : "invalid")
        appendRow("Topology local node", formatNodeStr(snapshot.topology.localNode))
        appendRow("PHY Reg00 physical_id", !phy.reg0Valid ? "not read (using OHCI NodeID)" : (phy.physicalId == 63 ? "63 / unassigned" : "\(phy.physicalId)"))
        appendRow("Verdict", cons.verdict)
        if cons.warnings.isEmpty && cons.notes.isEmpty {
            appendRow("Reason", "PHY-derived state is consistent with OHCI/topology.")
        }
        if !cons.warnings.isEmpty {
            report += "\nWarnings (trust order: OHCI NodeID > Self-ID topology > PHY Reg00):\n"
            for w in cons.warnings {
                report += "  ⚠ \(w.code)\n      \(w.detail)\n"
            }
        }
        if !cons.notes.isEmpty {
            report += "\nNotes (informational, not problems):\n"
            for n in cons.notes {
                report += "  ℹ \(n.code)\n      \(n.detail)\n"
            }
        }
        
        // --- Inbound CSR Statistics ---
        appendTitle("Inbound CSR Telemetry Stats (software-observed standard CSR only)")
        appendRow("Config ROM Reads", snapshot.inboundCSRStats.inboundConfigROMReads)
        appendRow("STATE_SET Writes", snapshot.inboundCSRStats.inboundStateSetWrites)
        appendRow("STATE_CLEAR Writes", snapshot.inboundCSRStats.inboundStateClearWrites)
        appendRow("Bus Manager ID Reads", snapshot.inboundCSRStats.inboundBusManagerIdReads)
        appendRow("Bus Manager ID Locks", snapshot.inboundCSRStats.inboundBusManagerIdLocks)
        appendRow("Bandwidth Register Reads", snapshot.inboundCSRStats.inboundBandwidthReads)
        appendRow("Bandwidth Register Locks", snapshot.inboundCSRStats.inboundBandwidthLocks)
        appendRow("Channel Register Reads", snapshot.inboundCSRStats.inboundChannelReads)
        appendRow("Channel Register Locks", snapshot.inboundCSRStats.inboundChannelLocks)
        appendRow("Broadcast Channel Reads", snapshot.inboundCSRStats.inboundBroadcastChannelReads)
        appendRow("Broadcast Channel Writes", snapshot.inboundCSRStats.inboundBroadcastChannelWrites)
        appendRow("Topology Map Reads", snapshot.inboundCSRStats.inboundTopologyMapReads)
        appendRow("Speed Map Reads", snapshot.inboundCSRStats.inboundSpeedMapReads)
        appendRow("Unsupported CSR Requests", snapshot.inboundCSRStats.unsupportedCSRRequests)
        appendRow("Dropped CSR Requests", snapshot.inboundCSRStats.droppedCSRRequests)
        
        // --- CSR Contract Owners ---
        appendTitle("CSR Contract Allocations")
        let entryCount = Int(min(snapshot.csrContract.entryCount, UInt32(ASFW_DIAG_MAX_CSR_ENTRIES)))
        let entries: [ASFWDiagCSREntry] = withUnsafeBytes(of: snapshot.csrContract.entries) { buffer in
            let bound = buffer.bindMemory(to: ASFWDiagCSREntry.self)
            return Array(bound)
        }
        
        if entryCount > 0 {
            report += "  " + pad("CSR Name", 24) + " " + pad("Offset Address", 16) + " "
                + pad("Owner", 12) + " " + pad("Reads", 12) + " " + pad("Writes", 12) + "\n"
            report += "  " + String(repeating: "─", count: 72) + "\n"
            
            for i in 0..<entryCount {
                let entry = entries[i]
                
                // Get the string name from C char array
                let nameStr = withUnsafeBytes(of: entry.name) { charPtr -> String in
                    if let base = charPtr.baseAddress?.assumingMemoryBound(to: CChar.self) {
                        return String(cString: base)
                    }
                    return "unknown"
                }
                
                let ownerStr: String
                switch entry.owner {
                case ASFWDiagCSROwnerOHCIHardware.rawValue: ownerStr = "Hardware"
                case ASFWDiagCSROwnerASFWSoftware.rawValue: ownerStr = "Software"
                case ASFWDiagCSROwnerOmittedAddressError.rawValue: ownerStr = "Omitted"
                case ASFWDiagCSROwnerPlanned.rawValue: ownerStr = "Planned"
                default: ownerStr = "Unknown"
                }
                
                let namePad = pad(nameStr, 24)
                let ownerPad = pad(ownerStr, 12)
                report += String(format: "  %@ 0x%08X       %@ %-12d %-12d\n",
                                 namePad,
                                 entry.offset,
                                 ownerPad,
                                 entry.readCount,
                                 entry.writeCount)
            }
        } else {
            report += "  No CSR allocations catalogued.\n"
        }
        
        // --- Async Transaction Trace ---
        appendTitle("Recent Async Transactions Trace")
        let eventCount = Int(min(snapshot.asyncTrace.eventCount, UInt32(ASFW_DIAG_MAX_ASYNC_EVENTS)))
        appendRow("Trace Event Count", eventCount)
        appendRow("Dropped Trace Events", snapshot.asyncTrace.droppedCount)
        
        let events: [ASFWDiagAsyncEvent] = withUnsafeBytes(of: snapshot.asyncTrace.events) { buffer in
            let bound = buffer.bindMemory(to: ASFWDiagAsyncEvent.self)
            return Array(bound)
        }
        
        if eventCount > 0 {
            report += "\nTransaction History (most recent last, Δt relative to oldest shown):\n"
            report += "  " + pad("Δt(us)", 10) + " " + pad("Dir", 3) + " " + pad("Ctx", 6) + " "
                + pad("TL", 3) + " " + pad("TCode", 8) + " " + pad("Speed", 5) + " "
                + pad("Src", 6) + " " + pad("Dst", 6) + " " + pad("Address", 14) + " "
                + pad("Ack", 9) + " " + pad("RCode", 9) + "\n"
            report += "  " + String(repeating: "─", count: 92) + "\n"

            let baseUs = Double(events[0].timestampNs) / 1000.0

            for i in 0..<eventCount {
                let event = events[i]
                let us = Double(event.timestampNs) / 1000.0
                // direction: 0 = RX (PacketRouter), 1 = TX (Submitter)
                let dirStr = event.direction == 1 ? "TX" : "RX"

                // The context field only encodes request(0)/response(1); the AR/AT axis is the
                // direction, so derive the label from both rather than the (ambiguous) raw value.
                let arat = event.direction == 1 ? "AT" : "AR"
                let reqrsp = event.context == 0 ? "Req" : (event.context == 1 ? "Rsp" : "?")
                let ctxStr = arat + reqrsp
                
                let speedStr: String
                switch event.speed {
                case ASFWDiagSpeedS100.rawValue: speedStr = "S100"
                case ASFWDiagSpeedS200.rawValue: speedStr = "S200"
                case ASFWDiagSpeedS400.rawValue: speedStr = "S400"
                case ASFWDiagSpeedS800.rawValue: speedStr = "S800"
                default: speedStr = "S?"
                }
                
                let ackStr: String
                if event.ackCode == 0xFF {
                    ackStr = "-"
                } else {
                    switch event.ackCode {
                    case 0x01: ackStr = "complete"
                    case 0x02: ackStr = "pending"
                    case 0x04: ackStr = "busy_X"
                    case 0x05: ackStr = "busy_A"
                    case 0x06: ackStr = "busy_B"
                    case 0x0D: ackStr = "data_err"
                    case 0x0E: ackStr = "type_err"
                    default: ackStr = String(format: "0x%02X", event.ackCode)
                    }
                }
                
                let rcodeStr: String
                if event.rCode == 0xFF {
                    rcodeStr = "-"
                } else {
                    switch event.rCode {
                    case 0: rcodeStr = "complete"
                    case 4: rcodeStr = "conflict"
                    case 5: rcodeStr = "data_err"
                    case 6: rcodeStr = "type_err"
                    case 7: rcodeStr = "addr_err"
                    default: rcodeStr = String(format: "0x%02X", event.rCode)
                    }
                }
                
                let tcodeStr: String
                switch event.tCode {
                case 0: tcodeStr = "WrQuad"
                case 1: tcodeStr = "WrBlock"
                case 2: tcodeStr = "WrResp"
                case 4: tcodeStr = "RdQuad"
                case 5: tcodeStr = "RdBlock"
                case 6: tcodeStr = "RdQResp"
                case 7: tcodeStr = "RdBResp"
                case 8: tcodeStr = "CycleSt"
                case 9: tcodeStr = "LockReq"
                case 10: tcodeStr = "PhyPkt"
                case 11: tcodeStr = "LockRsp"
                default: tcodeStr = String(format: "tC%d", event.tCode)
                }
                
                let addrStr = event.address != 0 ? String(format: "0x%012llX", event.address) : "-"
                // For TX the wire source is filled by hardware (event.sourceId == 0), so show the
                // local node; for RX the captured source/dest are the real wire IDs.
                let srcStr: String
                if event.direction == 1 {
                    let ln = snapshot.busContract.localNode
                    srcStr = ln <= 0x3E ? String(format: "0x%04X", 0xFFC0 | ln) : "self"
                } else {
                    srcStr = String(format: "0x%04X", event.sourceId)
                }
                report += "  " + pad(String(format: "%.1f", us - baseUs), 10) + " "
                    + pad(dirStr, 3) + " " + pad(ctxStr, 6) + " "
                    + pad(String(event.tLabel), 3) + " " + pad(tcodeStr, 8) + " "
                    + pad(speedStr, 5) + " "
                    + pad(srcStr, 6) + " "
                    + pad(String(format: "0x%04X", event.destinationId), 6) + " "
                    + pad(addrStr, 14) + " "
                    + pad(ackStr, 9) + " " + pad(rcodeStr, 9) + "\n"
            }
        } else {
            report += "  No asynchronous transaction events recorded.\n"
        }
        
        report += "\n" + String(repeating: "═", count: 60) + "\n"
        report += "END OF REPORT\n"

        return report
    }
}

// MARK: - PHY register decode + consistency validation
//
// IEEE 1394-2008 PHY page-0 base registers. Spec bit numbering is bit0=MSB..bit7=LSB, so a
// spec field at IEEE bits [a:b] maps to host byte bits [7-a : 7-b]. Note 0xFF is a *valid*
// decode (physical_id=63 / root / powered = unassigned, isolated PHY); whether it reflects
// reality is decided by the consistency check against OHCI NodeID / Self-ID topology, not by
// the value itself.

struct PhyDecode {
    let validMask: UInt32       // bit i => regs[i] read succeeded (OHCI rdDone)
    var reg0Valid: Bool { (validMask & 1) != 0 }
    let physicalId: UInt32      // Reg0 IEEE[0:5] -> std[7:2]
    let root: Bool              // Reg0 IEEE bit6 -> std1
    let powerStatus: Bool       // Reg0 IEEE bit7 -> std0
    let rootHoldoff: Bool       // Reg1 IEEE bit0 -> std7
    let initiateBusReset: Bool  // Reg1 IEEE bit1 -> std6
    let gapCount: UInt32        // Reg1 IEEE[2:7] -> std[5:0]
    let extended: UInt32        // Reg2 IEEE[0:2] -> std[7:5] (0b111 == extended map)
    let totalPorts: UInt32      // Reg2 IEEE[3:7] -> std[4:0]
    let maxSpeedField: UInt32   // Reg3 IEEE[0:2] -> std[7:5]
    let repeaterDelay: UInt32   // Reg3 IEEE[3:6] -> std[4:1]
    let linkOn: Bool            // Reg4 IEEE bit0 -> std7 (LCtrl)
    let contender: Bool         // Reg4 IEEE bit1 -> std6
    let jitter: UInt32          // Reg4 IEEE[2:4] -> std[5:3]
    let powerClass: UInt32      // Reg4 IEEE[5:7] -> std[2:0]
    let watchdog: Bool          // Reg5 IEEE bit0 -> std7
    let isbr: Bool              // Reg5 IEEE bit1 -> std6
    let loop: Bool              // Reg5 IEEE bit2 -> std5
    let powerFail: Bool         // Reg5 IEEE bit3 -> std4
    let timeout: Bool           // Reg5 IEEE bit4 -> std3
    let portEvent: Bool         // Reg5 IEEE bit5 -> std2
    let enableAccel: Bool       // Reg5 IEEE bit6 -> std1
    let enableMulti: Bool       // Reg5 IEEE bit7 -> std0
    let pageSelect: UInt32      // Reg6 IEEE[0:2] -> std[7:5]
    let portSelect: UInt32      // Reg6 IEEE[4:7] -> std[3:0]

    init(regs: [UInt32], validMask: UInt32) {
        self.validMask = validMask
        func reg(_ i: Int) -> UInt32 { i < regs.count ? (regs[i] & 0xFF) : 0xFF }
        let r0 = reg(0), r1 = reg(1), r2 = reg(2), r3 = reg(3), r4 = reg(4), r5 = reg(5), r6 = reg(6)
        physicalId = (r0 >> 2) & 0x3F
        root = ((r0 >> 1) & 1) == 1
        powerStatus = (r0 & 1) == 1
        rootHoldoff = ((r1 >> 7) & 1) == 1
        initiateBusReset = ((r1 >> 6) & 1) == 1
        gapCount = r1 & 0x3F
        extended = (r2 >> 5) & 0x7
        totalPorts = r2 & 0x1F
        maxSpeedField = (r3 >> 5) & 0x7
        repeaterDelay = (r3 >> 1) & 0x0F
        linkOn = ((r4 >> 7) & 1) == 1
        contender = ((r4 >> 6) & 1) == 1
        jitter = (r4 >> 3) & 0x7
        powerClass = r4 & 0x7
        watchdog = ((r5 >> 7) & 1) == 1
        isbr = ((r5 >> 6) & 1) == 1
        loop = ((r5 >> 5) & 1) == 1
        powerFail = ((r5 >> 4) & 1) == 1
        timeout = ((r5 >> 3) & 1) == 1
        portEvent = ((r5 >> 2) & 1) == 1
        enableAccel = ((r5 >> 1) & 1) == 1
        enableMulti = (r5 & 1) == 1
        pageSelect = (r6 >> 5) & 0x7
        portSelect = r6 & 0x0F
    }
}

enum PhyConsistencyChecker {
    struct Result {
        let verdict: String
        let warnings: [(code: String, detail: String)]  // genuine inconsistencies
        let notes: [(code: String, detail: String)]      // informational, not a problem
    }

    static func check(phy: PhyDecode, regs: [UInt32],
                      bus: ASFWDiagBusContract,
                      topo: ASFWDiagTopology,
                      ohci: ASFWDiagOHCI) -> Result {
        var warnings: [(code: String, detail: String)] = []
        var notes: [(code: String, detail: String)] = []

        let ohciNodeValid = (ohci.nodeId >> 31) & 1 == 1
        let ohciNode = ohci.nodeId & 0x3F
        let topoValid = topo.valid != 0
        let localIsRoot = topoValid && topo.localNode == topo.rootNode

        // Reg0 is redundant — the in-tree Linux reference never reads PHY reg 0; node identity
        // comes from OHCI NodeID. So a failed reg0 read is a benign NOTE, not a warning. When it
        // *did* read, its decoded id/root are real and any conflict with OHCI/topology matters.
        if !phy.reg0Valid {
            notes.append(("NOTE_PHY_REG00_NOT_READ",
                "Reg00 read did not complete (rdDone not set). Benign: reg 0 is redundant — node identity is taken from OHCI NodeID, and the Linux firewire-ohci reference never reads PHY reg 0 either."))
        } else {
            if ohciNodeValid && phy.physicalId != ohciNode {
                warnings.append(("WARN_PHY_ID_CONFLICT",
                    "PHY physical_id=\(phy.physicalId) != OHCI NodeID node=\(ohciNode). OHCI NodeID is authoritative."))
            }
            if phy.physicalId == 63 && topoValid {
                warnings.append(("WARN_PHY_UNASSIGNED_BUT_TOPOLOGY_VALID",
                    "Reg00 read OK and reports physical_id=63 (isolated), but Self-ID topology is valid — genuine contradiction, investigate the PHY."))
            }
            if phy.root != localIsRoot {
                warnings.append(("WARN_PHY_ROOT_CONFLICT",
                    "PHY root bit=\(phy.root) != topology localIsRoot=\(localIsRoot)."))
            }
        }

        // Regs 1–15 are read in normal operation; a failure there is unexpected → warning.
        let otherFailed = (0..<min(regs.count, 16)).filter { $0 != 0 && (phy.validMask & (1 << $0)) == 0 }
        if !otherFailed.isEmpty {
            warnings.append(("WARN_PHY_REG_READ_FAILED",
                "Unexpected PHY register read failure(s): \(otherFailed.map { "Reg\(String(format: "%02d", $0))" }.joined(separator: ", "))."))
        }

        if (phy.validMask & (1 << 1)) != 0 && phy.gapCount != bus.gapCount {
            warnings.append(("WARN_GAP_CONFLICT",
                "PHY gap_count=\(phy.gapCount) != bus gap_count=\(bus.gapCount)."))
        }
        if (phy.validMask & (1 << 5)) != 0 && phy.powerFail {
            notes.append(("NOTE_PWR_FAIL_LATCHED",
                "pwr_fail=1; this rcu bit latches on PHY power-up / cable-power loss and stays set until software clears it — not fatal on its own."))
        }
        if phy.pageSelect != 0 && phy.pageSelect != 1 {
            notes.append(("NOTE_VENDOR_PAGE",
                "page_select=\(phy.pageSelect); Regs 08–15 are vendor-dependent and not decoded as standard."))
        }

        let verdict = !warnings.isEmpty ? "WARNING" : (notes.isEmpty ? "OK" : "OK (with notes)")
        return Result(verdict: verdict, warnings: warnings, notes: notes)
    }
}
