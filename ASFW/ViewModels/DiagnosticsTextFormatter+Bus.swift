//
//  DiagnosticsTextFormatter+Bus.swift
//  ASFW
//
//  Report header, bus contract, role coordinator, and bus-manager/IRM runtime
//  sections. See DiagnosticsTextFormatter.format for the per-section-frame
//  rationale (avoids a single oversized stack frame on a small-stacked worker).
//

import Foundation

extension DiagnosticsTextFormatter {

    static func appendHeader(_ r: DiagnosticsReport,
                             _ snapshot: ASFWDiagnosticsSnapshot,
                             _ version: DriverVersionInfo?) {
        r.raw("ASFW 1394 DIAGNOSTICS REPORT\n")
        r.raw(String(repeating: "═", count: 60) + "\n")
        // The driver header timestamp is mach_absolute_time since SYSTEM BOOT, not
        // driver load — so this is system uptime, not driver uptime. Labeled
        // accordingly. (True per-load driver uptime lives in ControllerMetrics but
        // is not yet plumbed through the diagnostics ABI.)
        r.row("Report Generated", Date().description)
        r.row("System Uptime (since boot)", DiagFormat.uptime(snapshot.busContract.header.timestampNs))
        r.row("System Uptime (ns)", snapshot.busContract.header.timestampNs)
        // Loaded driver build — use this (not uptime) to confirm the running dext
        // matches the source you expect.
        if let v = version {
            r.row("Driver Build",
                  "v\(v.semanticVersion) (\(v.gitCommitShort)\(v.gitDirty ? " DIRTY" : "") @ \(v.gitBranch))")
        }
        r.row("ABI Version", snapshot.busContract.header.abiVersion)
        r.row("Generation", snapshot.busContract.header.generation)
        r.row("Snapshot Sequence", snapshot.busContract.header.snapshotSeq)
    }

    static func appendBusContract(_ r: DiagnosticsReport,
                                  _ snapshot: ASFWDiagnosticsSnapshot) {
        r.title("Bus Contract")
        r.row("Bus ID", snapshot.busContract.busId)
        r.row("Local Node", DiagFormat.nodeStr(snapshot.busContract.localNode))
        r.row("Root Node", DiagFormat.nodeStr(snapshot.busContract.rootNode))
        r.row("IRM Node", DiagFormat.nodeStr(snapshot.busContract.irmNode))
        r.row("BM Node", DiagFormat.nodeStr(snapshot.busContract.bmNode))
        r.row("Node Count", snapshot.busContract.nodeCount)
        r.row("Gap Count", snapshot.busContract.gapCount)
        // maxHops==0 is only valid for a single-node bus; on a multi-node bus it means the
        // parent/child adjacency wasn't built when the snapshot was taken (not computed).
        if snapshot.busContract.maxHops == 0 && snapshot.busContract.nodeCount > 1 {
            r.row("Max Hops", "unavailable (not computed)")
        } else {
            r.row("Max Hops", snapshot.busContract.maxHops)
        }
        r.row("Cycle Start Observed", snapshot.busContract.cycleStartObserved != 0 ? "Yes" : "No")
        r.row("Cycle Start Source Node", DiagFormat.nodeStr(snapshot.busContract.cycleStartSourceNode))
        r.row("Local Cycle Master Enabled", snapshot.busContract.localCycleMasterEnabled != 0 ? "Yes" : "No")
        r.row("Local Cycle Timer Enabled", snapshot.busContract.localCycleTimerEnabled != 0 ? "Yes" : "No")
        r.row("ASFW-Initiated Reset Count", snapshot.busContract.asfwInitiatedResetCount)

        // "Role Forcing Mode": whether the role-policy evaluator forces the local
        // node's root standing. "Standard" = no forcing (normal election); it is
        // independent of BM/IRM participation, so it does not contradict a
        // "Client Only" Configured Role Mode.
        let policyStr: String
        switch snapshot.busContract.rolePolicyMode {
        case 0: policyStr = "Standard (no forced root)"
        case 1: policyStr = "Force Root"
        case 2: policyStr = "Force Not Root"
        default: policyStr = "Unknown (\(snapshot.busContract.rolePolicyMode))"
        }
        r.row("Role Forcing Mode", policyStr)

        r.row("Role Verdict (last action)", DiagFormat.roleAction(snapshot.busContract.roleVerdict))
    }

    static func appendRoleCoordinator(_ r: DiagnosticsReport,
                                      _ snapshot: ASFWDiagnosticsSnapshot) {
        r.title("Role Coordinator Policy Engine")
        let rcModeStr: String
        switch snapshot.roleCoordinator.policyMode {
        case 0: rcModeStr = "Standard (no forced root)"
        case 1: rcModeStr = "Force Root"
        case 2: rcModeStr = "Force Not Root"
        default: rcModeStr = "Unknown (\(snapshot.roleCoordinator.policyMode))"
        }
        r.row("Role Forcing Mode", rcModeStr)
        r.row("Last Decision", DiagFormat.roleAction(snapshot.roleCoordinator.lastDecision))
        r.row("Last Action", DiagFormat.roleAction(snapshot.roleCoordinator.lastAction))
        r.row("Last Reset Flavor", DiagFormat.roleReset(snapshot.roleCoordinator.lastActionResult))
        // These two are sourced from the ROOT node's BIB cycle-master evidence, NOT the local
        // node's enable state — relabel to avoid the contradiction with the authoritative
        // hardware bit (shown from the Bus Contract / OHCI LinkControl).
        r.row("Root CMC Known (evidence)", snapshot.roleCoordinator.localCycleMasterAllowed != 0 ? "Yes" : "No")
        r.row("Root CMC Capable (evidence)", snapshot.roleCoordinator.localCycleMasterEnabled != 0 ? "Yes" : "No")
        r.row("Local Cycle Master Enabled (HW)", snapshot.busContract.localCycleMasterEnabled != 0 ? "Yes" : "No")

        // The CMSTR fields are only meaningful when a remote-cycle-master write was actually
        // issued. When unset (all zero) render "none" rather than a misleading node 0 / addr 0.
        let cmstrExecuted = snapshot.roleCoordinator.remoteCMSTRAddress != 0
            || snapshot.roleCoordinator.remoteCMSTRResult != 0
            || snapshot.roleCoordinator.remoteCMSTRPayload != 0
        if cmstrExecuted {
            r.row("Remote CMSTR Target Node", DiagFormat.nodeStr(snapshot.roleCoordinator.remoteCMSTRTargetNode))
            r.row("Remote CMSTR Result", DiagFormat.hex32(snapshot.roleCoordinator.remoteCMSTRResult))
            r.row("Remote CMSTR Address", DiagFormat.hex64(snapshot.roleCoordinator.remoteCMSTRAddress))
            r.row("Remote CMSTR Payload", DiagFormat.hex32(snapshot.roleCoordinator.remoteCMSTRPayload))
            r.row("Remote CMSTR RCode", snapshot.roleCoordinator.remoteCMSTRRCode)
        } else {
            r.row("Remote CMSTR Policy", "observe-only / not armed")
            r.row("Reason", "ClientOnly / ObserveOnly; remote CMSTR disabled by policy")
        }
        r.row("Cycle Start Observed", snapshot.roleCoordinator.cycleStartObserved != 0 ? "Yes" : "No")
        r.row("Cycle Start Source Node", DiagFormat.nodeStr(snapshot.roleCoordinator.cycleStartSourceNode))
        r.row("Reset Guard Active", snapshot.roleCoordinator.resetGuardActive != 0 ? "Yes" : "No")
        r.row("BM Retry Count", snapshot.roleCoordinator.bmRetryCount)
        r.row("Gap Mismatch Detected", snapshot.roleCoordinator.gapMismatchDetected != 0 ? "Yes" : "No")
    }

    static func appendBusManagerRuntime(_ r: DiagnosticsReport,
                                        _ snapshot: ASFWDiagnosticsSnapshot) {
        r.title("Bus Manager & IRM Role Runtime")
        let bmModeStr: String
        switch snapshot.busManager.roleMode {
        case 0: bmModeStr = "Legacy BMC Cleared"
        case 1: bmModeStr = "Client Only (no BM/IRM)"
        case 2: bmModeStr = "IRM Server Only"
        case 3: bmModeStr = "Full Bus Manager"
        default: bmModeStr = "Unknown (\(snapshot.busManager.roleMode))"
        }
        r.row("Configured Role Mode", bmModeStr)
        r.row("Advertised BMC", snapshot.busManager.advertisedBmc != 0 ? "Yes" : "No")
        r.row("Advertised IRMC", snapshot.busManager.advertisedIrmc != 0 ? "Yes" : "No")
        r.row("Advertised CMC", snapshot.busManager.advertisedCmc != 0 ? "Yes" : "No")
        r.row("Advertised ISC", snapshot.busManager.advertisedIsc != 0 ? "Yes" : "No")

        r.row("Local Node is IRM", snapshot.busManager.localIsIRM != 0 ? "Yes" : "No")
        r.row("Local Node is BM", snapshot.busManager.localIsBM != 0 ? "Yes" : "No")
        r.row("Local Node is Root", snapshot.busManager.localIsRoot != 0 ? "Yes" : "No")

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
        r.row("BM Owner Source", bmOwnerStr)
        r.row("Last BM ID Value", DiagFormat.nodeStr(snapshot.busManager.lastBusManagerIdOldValue))
        r.row("Stale Election Aborts", snapshot.busManager.staleElectionAbortCount)
        r.row("Failed Elections", snapshot.busManager.failedElectionCount)
        r.row("Unexpected software resource accesses", snapshot.busManager.unexpectedResourceCsrSoftwareCount)

        let irmStateStr: String
        switch snapshot.busManager.localIrmResourceState {
        case 0: irmStateStr = "disabled"
        case 1: irmStateStr = "initialized"
        case 2: irmStateStr = "failed"
        default: irmStateStr = "Unknown"
        }
        r.row("Local IRM Resource State", irmStateStr)
        r.row("Local IRM Readback Valid", snapshot.busManager.localIrmReadbackValid != 0 ? "Yes" : "No")

        let csrStatusStr: String
        switch snapshot.busManager.csrControlLastStatus {
        case 0: csrStatusStr = "OK"
        case 1: csrStatusStr = "Timeout"
        case 2: csrStatusStr = "HardwareUnavailable"
        case 3: csrStatusStr = "AccessFailed"
        default: csrStatusStr = "Unknown"
        }
        r.row("CSRControl Last Status", csrStatusStr)

        r.row("BUS_MANAGER_ID local", DiagFormat.nodeStr(snapshot.busManager.localIrmBusManagerId))
        r.row("BANDWIDTH_AVAILABLE local", snapshot.busManager.localIrmBandwidthAvailable)
        r.row("CHANNELS_AVAILABLE_HI local", DiagFormat.hex32(snapshot.busManager.localIrmChannelsAvailableHi))
        r.row("CHANNELS_AVAILABLE_LO local", DiagFormat.hex32(snapshot.busManager.localIrmChannelsAvailableLo))

        r.row("Topology Map Valid", snapshot.busManager.topologyMapValid != 0 ? "Yes" : "No")
        r.row("Topology Map CSR Generation", snapshot.busManager.topologyMapCSRGeneration)
        r.row("Topology Map Self-ID Count", snapshot.busManager.topologyMapSelfIdCount)
        r.row("Topology Map CRC", DiagFormat.hex16(snapshot.busManager.topologyMapCRC))
        r.row("Topology Map DMA Ready", snapshot.busManager.topologyMapDMAReady != 0 ? "Yes" : "No")

        // Pass 1 & 3 Evidence & Verdict Output
        let verdictStr: String
        switch snapshot.busManager.bmPolicyVerdict {
        case 0: verdictStr = "ObserveOnly"
        case 1: verdictStr = "RemoteRootAlreadyCycling"
        case 2: verdictStr = "RemoteCMSTRNeeded"
        case 3: verdictStr = "LocalRootCycleMaster"
        default: verdictStr = "Unknown (\(snapshot.busManager.bmPolicyVerdict))"
        }
        r.row("BM Policy Verdict", verdictStr)
        r.row("Root CMC Known", snapshot.busManager.rootCmcKnown != 0 ? "Yes" : "No")
        r.row("Root CMC Capable", snapshot.busManager.rootCmcCapable != 0 ? "Yes" : "No")
        r.row("CycleStart Observed", snapshot.busManager.cycleStartObserved != 0 ? "Yes" : "No")
        r.row("CycleStart Source", DiagFormat.nodeStr(snapshot.busManager.cycleStartSourceNode))
        r.row("Remote CMSTR Allowed", snapshot.busManager.remoteCmstrAllowed != 0 ? "Yes" : "No")

        let cmstrActionStr: String
        if snapshot.busManager.remoteCmstrAllowed != 0 {
            if snapshot.busManager.lastRemoteCmstrResult == 0 {
                cmstrActionStr = "success"
            } else {
                cmstrActionStr = "failed (code \(snapshot.busManager.lastRemoteCmstrResult))"
            }
        } else {
            cmstrActionStr = "not armed"
        }
        r.row("Remote CMSTR Action", cmstrActionStr)
    }
}
