//
//  PhyDiagnostics.swift
//  ASFW
//
//  PHY register decode + consistency validation, split out of
//  DiagnosticsTextFormatter.
//
// IEEE 1394-2008 PHY page-0 base registers. Spec bit numbering is bit0=MSB..bit7=LSB, so a
// spec field at IEEE bits [a:b] maps to host byte bits [7-a : 7-b]. Note 0xFF is a *valid*
// decode (physical_id=63 / root / powered = unassigned, isolated PHY); whether it reflects
// reality is decided by the consistency check against OHCI NodeID / Self-ID topology, not by
// the value itself.

import Foundation

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
