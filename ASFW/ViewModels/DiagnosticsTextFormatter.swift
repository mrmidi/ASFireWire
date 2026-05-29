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
        
        func appendRow(_ label: String, _ value: Any) {
            report += String(format: "%-30s %@\n", (label + ":").cString(using: .utf8) ?? [], String(describing: value))
        }
        
        func formatNodeStr(_ nodeVal: UInt32) -> String {
            if nodeVal == 0xFFFFFFFF || nodeVal == 0x3F {
                return "unknown"
            }
            return String(format: "node %d / 0x%04X", nodeVal, 0xFFC0 + nodeVal)
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
        let timestamp = Date(timeIntervalSince1970: Double(snapshot.busContract.header.timestampNs) / 1_000_000_000.0)
        appendRow("Timestamp", timestamp.description)
        appendRow("Uptime Nanoseconds", snapshot.busContract.header.timestampNs)
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
        appendRow("Max Hops", snapshot.busContract.maxHops)
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
        
        let verdictStr: String
        switch snapshot.busContract.roleVerdict {
        case 0: verdictStr = "Undecided"
        case 1: verdictStr = "Standard Leaf/Branch"
        case 2: verdictStr = "Acquired Root"
        case 3: verdictStr = "Yielded Root"
        case 4: verdictStr = "Conflict Detected"
        default: verdictStr = "Unknown (\(snapshot.busContract.roleVerdict))"
        }
        appendRow("Role Verdict", verdictStr)
        
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
            report += String(format: "  %-8s %-7s %-7s %-10s %-5s %-6s %-10s %-8s\n", "NodeID", "Local", "Root", "Contender", "Speed", "Power", "LinkActive", "Ports")
            report += "  " + String(repeating: "─", count: 68) + "\n"
            
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
                
                let portDetailsPad = portDetails.padding(toLength: 8, withPad: " ", startingAt: 0)
                report += String(format: "  %-8d %-7s %-7s %-10s %-5s %-6d %-10s %@\n",
                                 node.nodeId,
                                 node.isLocal != 0 ? "Yes" : "No",
                                 node.isRoot != 0 ? "Yes" : "No",
                                 node.contender != 0 ? "Yes" : "No",
                                 speedStr,
                                 node.powerClass,
                                 node.linkActive != 0 ? "Yes" : "No",
                                 portDetailsPad)
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
        appendRow("Last Decision", formatHex32(snapshot.roleCoordinator.lastDecision))
        appendRow("Last Action", formatHex32(snapshot.roleCoordinator.lastAction))
        appendRow("Last Action Result", formatHex32(snapshot.roleCoordinator.lastActionResult))
        appendRow("Local Cycle Master Allowed", snapshot.roleCoordinator.localCycleMasterAllowed != 0 ? "Yes" : "No")
        appendRow("Local Cycle Master Enabled", snapshot.roleCoordinator.localCycleMasterEnabled != 0 ? "Yes" : "No")
        appendRow("Remote CMSTR Target Node", formatNodeStr(snapshot.roleCoordinator.remoteCMSTRTargetNode))
        appendRow("Remote CMSTR Result", formatHex32(snapshot.roleCoordinator.remoteCMSTRResult))
        appendRow("Remote CMSTR Address", formatHex64(snapshot.roleCoordinator.remoteCMSTRAddress))
        appendRow("Remote CMSTR Payload", formatHex32(snapshot.roleCoordinator.remoteCMSTRPayload))
        appendRow("Remote CMSTR RCode", snapshot.roleCoordinator.remoteCMSTRRCode)
        appendRow("Cycle Start Observed", snapshot.roleCoordinator.cycleStartObserved != 0 ? "Yes" : "No")
        appendRow("Cycle Start Source Node", formatNodeStr(snapshot.roleCoordinator.cycleStartSourceNode))
        appendRow("Reset Guard Active", snapshot.roleCoordinator.resetGuardActive != 0 ? "Yes" : "No")
        appendRow("BM Retry Count", snapshot.roleCoordinator.bmRetryCount)
        appendRow("Gap Mismatch Detected", snapshot.roleCoordinator.gapMismatchDetected != 0 ? "Yes" : "No")
        
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
            report += "\nPHY Registers:\n"
            for r in 0..<regCount {
                report += String(format: "  Reg %02d: 0x%02X\n", r, regs[r])
            }
        }
        
        // --- Inbound CSR Statistics ---
        appendTitle("Inbound CSR Telemetry Stats")
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
            report += String(format: "  %-24s %-16s %-12s %-12s %-12s\n", "CSR Name", "Offset Address", "Owner", "Reads", "Writes")
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
                
                let namePad = nameStr.padding(toLength: 24, withPad: " ", startingAt: 0)
                let ownerPad = ownerStr.padding(toLength: 12, withPad: " ", startingAt: 0)
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
            report += "\nTransaction History (most recent last):\n"
            report += String(format: "  %-8s %-5s %-7s %-5s %-5s %-5s %-6s %-8s %-6s %-6s %-6s\n",
                             "Time(us)", "Dir", "Ctx", "TLabel", "TCode", "Speed", "Src", "Dst", "Addr", "Ack", "RCode")
            report += "  " + String(repeating: "─", count: 80) + "\n"
            
            for i in 0..<eventCount {
                let event = events[i]
                let us = Double(event.timestampNs) / 1000.0
                let dirStr = event.direction == 0 ? "TX" : "RX"
                
                let ctxStr: String
                switch event.context {
                case 0: ctxStr = "AR_Req"
                case 1: ctxStr = "AR_Rsp"
                case 2: ctxStr = "AT_Req"
                case 3: ctxStr = "AT_Rsp"
                default: ctxStr = "Ctx\(event.context)"
                }
                
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
                
                let dirPad = dirStr.padding(toLength: 5, withPad: " ", startingAt: 0)
                let ctxPad = ctxStr.padding(toLength: 7, withPad: " ", startingAt: 0)
                let tcodePad = tcodeStr.padding(toLength: 6, withPad: " ", startingAt: 0)
                let speedPad = speedStr.padding(toLength: 5, withPad: " ", startingAt: 0)
                let ackPad = ackStr.padding(toLength: 6, withPad: " ", startingAt: 0)
                let rcodePad = rcodeStr.padding(toLength: 6, withPad: " ", startingAt: 0)
                
                report += String(format: "  %-8.1f %@ %@ %-6d %@ %@ 0x%02X   0x%02X   0x%04X %@ %@\n",
                                 us, dirPad, ctxPad, event.tLabel, tcodePad, speedPad,
                                 event.sourceId & 0x3F, event.destinationId & 0x3F,
                                 event.address & 0xFFFF, ackPad, rcodePad)
            }
        } else {
            report += "  No asynchronous transaction events recorded.\n"
        }
        
        report += "\n" + String(repeating: "═", count: 60) + "\n"
        report += "END OF REPORT\n"
        
        return report
    }
}
