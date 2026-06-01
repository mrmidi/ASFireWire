//
//  DiagnosticsTextFormatter+Hardware.swift
//  ASFW
//
//  OHCI link/controller snapshot, PHY interface/interpretation/consistency,
//  inbound CSR telemetry, CSR contract allocations, and the async transaction
//  trace.
//

import Foundation

extension DiagnosticsTextFormatter {

    static func appendOHCI(_ r: DiagnosticsReport,
                           _ snapshot: ASFWDiagnosticsSnapshot) {
        r.title("OHCI Link/Controller Snapshot")
        r.row("OHCI Version", DiagFormat.hex32(snapshot.ohci.version))
        r.row("GUID ROM Present", snapshot.ohci.guidROM != 0 ? "Yes" : "No")
        r.row("AT Tx Retries", snapshot.ohci.atRetries)
        r.row("CSR Control Register", DiagFormat.hex32(snapshot.ohci.csrControl))
        r.row("Config ROM Header Reg", DiagFormat.hex32(snapshot.ohci.configROMHeader))
        r.row("Bus Options Register", DiagFormat.hex32(snapshot.ohci.busOptions))
        r.row("Node ID Register", DiagFormat.hex32(snapshot.ohci.nodeId))
        r.row("PHY Control Register", DiagFormat.hex32(snapshot.ohci.phyControl))
        r.row("Cycle Timer Register", DiagFormat.hex32(snapshot.ohci.isochronousCycleTimer))
        r.row("GUID (64-bit Hex)", String(format: "0x%08X%08X", snapshot.ohci.guidHi, snapshot.ohci.guidLo))
        r.row("HC Control Register (Set)", DiagFormat.hex32(snapshot.ohci.hcControlSet))
        r.row("Link Control Register (Set)", DiagFormat.hex32(snapshot.ohci.linkControlSet))
        r.row("Interrupt Event (Active)", DiagFormat.hex32(snapshot.ohci.intEventSet))
        r.row("Interrupt Mask (Enabled)", DiagFormat.hex32(snapshot.ohci.intMaskSet))
        r.row("Self-ID Buffer Pointer", DiagFormat.hex32(snapshot.ohci.selfIdBuffer))
        r.row("Self-ID Count Register", DiagFormat.hex32(snapshot.ohci.selfIdCount))
    }

    static func appendPHY(_ r: DiagnosticsReport,
                          _ snapshot: ASFWDiagnosticsSnapshot) {
        // --- PHY Status ---
        r.title("PHY Interface Snapshot")
        r.row("Link On", snapshot.phy.linkOn != 0 ? "Yes" : "No")
        r.row("Contender", snapshot.phy.contender != 0 ? "Yes" : "No")
        r.row("PHY Gap Count", snapshot.phy.gapCount)
        r.row("Last PhyConfig Root ID", DiagFormat.nodeStr(snapshot.phy.lastPhyConfigRootId))
        r.row("Last PhyConfig Gap Count", snapshot.phy.lastPhyConfigGapCount)
        r.row("Last PHY Reset Reason", snapshot.phy.lastPhyResetReason)

        let regs: [UInt32] = withUnsafeBytes(of: snapshot.phy.regs) { buffer in
            let bound = buffer.bindMemory(to: UInt32.self)
            return Array(bound)
        }
        let regCount = Int(min(snapshot.phy.regCount, UInt32(ASFW_DIAG_MAX_PHY_REGS)))
        if regCount > 0 {
            r.raw("\nPHY Registers (raw):\n")
            for reg in 0..<regCount {
                r.raw(String(format: "  Reg %02d: 0x%02X\n", reg, regs[reg]))
            }
        }

        // --- PHY Interpretation (decode + consistency live in PhyDiagnostics) ---
        let phy = PhyDecode(regs: regs, validMask: snapshot.phy.regValidMask)
        r.title("PHY Interpretation")
        // regValidMask tells us which reads actually succeeded (OHCI rdDone). With it we can
        // distinguish a genuine 0xFF (isolated PHY: physical_id=63) from a failed/timed-out read.
        if !phy.reg0Valid {
            // Benign: reg 0 is redundant. The in-tree Linux reference never reads PHY reg 0 —
            // node identity comes from OHCI NodeID. Source it from there instead of alarming.
            let ohciNode = snapshot.ohci.nodeId & 0x3F
            let ohciRoot = (snapshot.ohci.nodeId >> 30) & 1 == 1
            r.row("Reg00", "not read (rdDone not set) — reg 0 is redundant; identity from OHCI NodeID")
            r.row("Reg00 physical_id", "\(ohciNode) (from OHCI NodeID)")
            r.row("Reg00 root", "\(ohciRoot ? "Yes" : "No") (from OHCI NodeID)")
            r.row("Reg00 power_status", "n/a (reg 0 not read)")
        } else {
            r.row("Reg00 physical_id", phy.physicalId == 63 ? "63 / unassigned (isolated)" : "\(phy.physicalId)")
            r.row("Reg00 root", phy.root ? "Yes" : "No")
            r.row("Reg00 power_status", phy.powerStatus ? "Yes" : "No")
        }
        r.row("Reg01 root_holdoff", phy.rootHoldoff ? "Yes" : "No")
        r.row("Reg01 initiate_reset", phy.initiateBusReset ? "Yes" : "No")
        r.row("Reg01 gap_count", phy.gapCount)
        r.row("Reg02 extended", phy.extended == 7 ? "Yes (0b111)" : "\(phy.extended)")
        r.row("Reg02 total_ports", phy.totalPorts)
        r.row("Reg03 max_speed_field", phy.maxSpeedField)
        r.row("Reg03 repeater_delay", "\(phy.repeaterDelay) (~\(144 + Int(phy.repeaterDelay) * 20) ns)")
        r.row("Reg04 link_on", phy.linkOn ? "Yes" : "No")
        r.row("Reg04 contender", phy.contender ? "Yes" : "No")
        r.row("Reg04 jitter", phy.jitter)
        r.row("Reg04 power_class", "\(phy.powerClass) / \(DiagFormat.powerClass(phy.powerClass))")
        r.row("Reg05 pwr_fail", phy.powerFail ? "Yes (may be latched, verify)" : "No")
        r.row("Reg05 loop", phy.loop ? "Yes" : "No")
        r.row("Reg05 timeout", phy.timeout ? "Yes" : "No")
        r.row("Reg05 port_event", phy.portEvent ? "Yes" : "No")
        r.row("Reg06 page_select", phy.pageSelect)
        r.row("Reg06 port_select", phy.portSelect)

        // --- PHY Consistency vs OHCI NodeID / Self-ID topology ---
        let cons = PhyConsistencyChecker.check(phy: phy, regs: regs,
                                               bus: snapshot.busContract,
                                               topo: snapshot.topology,
                                               ohci: snapshot.ohci)
        r.title("PHY Consistency")
        let ohciNodeValid = (snapshot.ohci.nodeId >> 31) & 1 == 1
        let ohciNode = snapshot.ohci.nodeId & 0x3F
        r.row("OHCI NodeID (authoritative)", ohciNodeValid ? "valid, \(DiagFormat.nodeStr(ohciNode))" : "invalid")
        r.row("Topology local node", DiagFormat.nodeStr(snapshot.topology.localNode))
        r.row("PHY Reg00 physical_id", !phy.reg0Valid ? "not read (using OHCI NodeID)" : (phy.physicalId == 63 ? "63 / unassigned" : "\(phy.physicalId)"))
        r.row("Verdict", cons.verdict)
        if cons.warnings.isEmpty && cons.notes.isEmpty {
            r.row("Reason", "PHY-derived state is consistent with OHCI/topology.")
        }
        if !cons.warnings.isEmpty {
            r.raw("\nWarnings (trust order: OHCI NodeID > Self-ID topology > PHY Reg00):\n")
            for w in cons.warnings {
                r.raw("  ⚠ \(w.code)\n      \(w.detail)\n")
            }
        }
        if !cons.notes.isEmpty {
            r.raw("\nNotes (informational, not problems):\n")
            for n in cons.notes {
                r.raw("  ℹ \(n.code)\n      \(n.detail)\n")
            }
        }
    }

    static func appendInboundCSRStats(_ r: DiagnosticsReport,
                                      _ snapshot: ASFWDiagnosticsSnapshot) {
        r.title("Inbound CSR Telemetry Stats (software-observed standard CSR only)")
        r.row("Config ROM Reads", snapshot.inboundCSRStats.inboundConfigROMReads)
        r.row("STATE_SET Writes", snapshot.inboundCSRStats.inboundStateSetWrites)
        r.row("STATE_CLEAR Writes", snapshot.inboundCSRStats.inboundStateClearWrites)
        r.row("Bus Manager ID Reads", snapshot.inboundCSRStats.inboundBusManagerIdReads)
        r.row("Bus Manager ID Locks", snapshot.inboundCSRStats.inboundBusManagerIdLocks)
        r.row("Bandwidth Register Reads", snapshot.inboundCSRStats.inboundBandwidthReads)
        r.row("Bandwidth Register Locks", snapshot.inboundCSRStats.inboundBandwidthLocks)
        r.row("Channel Register Reads", snapshot.inboundCSRStats.inboundChannelReads)
        r.row("Channel Register Locks", snapshot.inboundCSRStats.inboundChannelLocks)
        r.row("Broadcast Channel Reads", snapshot.inboundCSRStats.inboundBroadcastChannelReads)
        r.row("Broadcast Channel Writes", snapshot.inboundCSRStats.inboundBroadcastChannelWrites)
        r.row("Topology Map Reads", snapshot.inboundCSRStats.inboundTopologyMapReads)
        r.row("Speed Map Reads", snapshot.inboundCSRStats.inboundSpeedMapReads)
        r.row("Unsupported CSR Requests", snapshot.inboundCSRStats.unsupportedCSRRequests)
        r.row("Dropped CSR Requests", snapshot.inboundCSRStats.droppedCSRRequests)
    }

    static func appendCSRContract(_ r: DiagnosticsReport,
                                  _ snapshot: ASFWDiagnosticsSnapshot) {
        let pad = DiagnosticsReport.pad

        r.title("CSR Contract Allocations")
        let entryCount = Int(min(snapshot.csrContract.entryCount, UInt32(ASFW_DIAG_MAX_CSR_ENTRIES)))
        let entries: [ASFWDiagCSREntry] = withUnsafeBytes(of: snapshot.csrContract.entries) { buffer in
            let bound = buffer.bindMemory(to: ASFWDiagCSREntry.self)
            return Array(bound)
        }

        if entryCount > 0 {
            r.raw("  " + pad("CSR Name", 24) + " " + pad("Offset Address", 16) + " "
                + pad("Owner", 12) + " " + pad("Reads", 12) + " " + pad("Writes", 12) + "\n")
            r.raw("  " + String(repeating: "─", count: 72) + "\n")

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
                r.raw(String(format: "  %@ 0x%08X       %@ %-12d %-12d\n",
                             namePad,
                             entry.offset,
                             ownerPad,
                             entry.readCount,
                             entry.writeCount))
            }
        } else {
            r.raw("  No CSR allocations catalogued.\n")
        }
    }

    static func appendAsyncTrace(_ r: DiagnosticsReport,
                                 _ snapshot: ASFWDiagnosticsSnapshot) {
        let pad = DiagnosticsReport.pad

        r.title("Recent Async Transactions Trace")
        let eventCount = Int(min(snapshot.asyncTrace.eventCount, UInt32(ASFW_DIAG_MAX_ASYNC_EVENTS)))
        r.row("Trace Event Count", eventCount)
        r.row("Dropped Trace Events", snapshot.asyncTrace.droppedCount)

        let events: [ASFWDiagAsyncEvent] = withUnsafeBytes(of: snapshot.asyncTrace.events) { buffer in
            let bound = buffer.bindMemory(to: ASFWDiagAsyncEvent.self)
            return Array(bound)
        }

        if eventCount > 0 {
            r.raw("\nTransaction History (most recent last, Δt relative to oldest shown):\n")
            r.raw("  " + pad("Δt(us)", 10) + " " + pad("Dir", 3) + " " + pad("Ctx", 6) + " "
                + pad("TL", 3) + " " + pad("TCode", 8) + " " + pad("Speed", 5) + " "
                + pad("Src", 6) + " " + pad("Dst", 6) + " " + pad("Address", 14) + " "
                + pad("Ack", 9) + " " + pad("RCode", 9) + "\n")
            r.raw("  " + String(repeating: "─", count: 92) + "\n")

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

                let speedStr = DiagFormat.speed4(event.speed)

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
                r.raw("  " + pad(String(format: "%.1f", us - baseUs), 10) + " "
                    + pad(dirStr, 3) + " " + pad(ctxStr, 6) + " "
                    + pad(String(event.tLabel), 3) + " " + pad(tcodeStr, 8) + " "
                    + pad(speedStr, 5) + " "
                    + pad(srcStr, 6) + " "
                    + pad(String(format: "0x%04X", event.destinationId), 6) + " "
                    + pad(addrStr, 14) + " "
                    + pad(ackStr, 9) + " " + pad(rcodeStr, 9) + "\n")
            }
        } else {
            r.raw("  No asynchronous transaction events recorded.\n")
        }
    }
}
