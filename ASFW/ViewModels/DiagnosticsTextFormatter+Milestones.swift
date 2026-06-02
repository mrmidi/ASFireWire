//
//  DiagnosticsTextFormatter+Milestones.swift
//  ASFW
//
//  Bus-manager policy milestone sections (M3 election … M9 CSR compliance) plus
//  post-reset timing. All read from snapshot.busManager / snapshot.postResetTiming.
//

import Foundation

extension DiagnosticsTextFormatter {

    // --- Milestone 3: Bus Manager Election ---
    static func appendBMElection(_ r: DiagnosticsReport,
                                 _ snapshot: ASFWDiagnosticsSnapshot) {
        r.title("Bus Manager Election (Milestone 3)")

        let candidateClassStr: String
        switch snapshot.busManager.bmCandidateClass {
        case 0: candidateClassStr = "Not a candidate"
        case 1: candidateClassStr = "Incumbent"
        case 2: candidateClassStr = "Non-incumbent (challenger)"
        default: candidateClassStr = "Unknown (\(snapshot.busManager.bmCandidateClass))"
        }
        r.row("BM Candidate Class", candidateClassStr)
        r.row("Incumbent BM Local Flag", snapshot.busManager.bmElectionLocalFlag != 0 ? "Yes" : "No")

        let electActionStr: String
        switch snapshot.busManager.bmElectionAction {
        case 0: electActionStr = "none (DoNotContend)"
        case 1: electActionStr = "Immediate (incumbent retry)"
        case 2: electActionStr = "Grace Period (125ms challenger delay)"
        case 3: electActionStr = "Yielded (fast-reset storm guard)"
        default: electActionStr = "Unknown (\(snapshot.busManager.bmElectionAction))"
        }
        r.row("Election Decision Action", electActionStr)

        r.row("Election In-Flight", snapshot.busManager.bmElectionState != 0 ? "Yes" : "No")

        let pathStr: String
        switch snapshot.busManager.bmElectionPath {
        case 0: pathStr = "none"
        case 1: pathStr = "Local OHCI CSRControl"
        case 2: pathStr = "Remote Async Lock (S100)"
        default: pathStr = "Unknown (\(snapshot.busManager.bmElectionPath))"
        }
        r.row("Election Attempt Path", pathStr)

        r.row("Election Compare Value", DiagFormat.hex32(snapshot.busManager.bmElectionCompareValue))
        r.row("Election Swap Value", DiagFormat.hex32(snapshot.busManager.bmElectionSwapValue))
        r.row("Last BUS_MANAGER_ID Old Value", DiagFormat.hex32(snapshot.busManager.lastBusManagerIdOldValue))

        r.row("Election Attempted Gen", snapshot.busManager.bmElectionAttemptedGen)
        r.row("Attempts This Gen", snapshot.busManager.bmElectionAttemptsThisGen)
        r.row("Stale Election Abort Count", snapshot.busManager.staleElectionAbortCount)
        r.row("Failed Election Count", snapshot.busManager.failedElectionCount)

        let activeMutationStr: String
        switch snapshot.busManager.fullBMActivityLevel {
        case 0: activeMutationStr = "none (ObserveOnly)"
        case 1: activeMutationStr = "BUS_MANAGER_ID only"
        case 2: activeMutationStr = "BUS_MANAGER_ID + cycle policy"
        case 3: activeMutationStr = "BUS_MANAGER_ID + cycle + gap policy"
        case 4: activeMutationStr = "BUS_MANAGER_ID + cycle + root/gap policy"
        case 5: activeMutationStr = "BUS_MANAGER_ID + cycle + root/gap + legacy remote CMSTR"
        default: activeMutationStr = "Unknown (\(snapshot.busManager.fullBMActivityLevel))"
        }
        r.row("Active BM Mutations", activeMutationStr)
    }

    // --- Milestone 4: IRM Fallback Planner ---
    static func appendIRMFallback(_ r: DiagnosticsReport,
                                  _ snapshot: ASFWDiagnosticsSnapshot) {
        r.title("IRM Fallback Planner (Milestone 4)")

        let fallbackStateStr: String
        switch snapshot.busManager.irmFallbackState {
        case 0: fallbackStateStr = "Disabled"
        case 1: fallbackStateStr = "WaitingForTopology"
        case 2: fallbackStateStr = "NotLocalIRM"
        case 3: fallbackStateStr = "WaitingForAnnexHGate"
        case 4: fallbackStateStr = "ProbingBusManagerId"
        case 5: fallbackStateStr = "BMExists"
        case 6: fallbackStateStr = "NoBMDetected"
        case 7: fallbackStateStr = "PlanningCycleRepair"
        case 8: fallbackStateStr = "ActionSuppressedByPolicy"
        case 9: fallbackStateStr = "SuppressedByTopology"
        case 10: fallbackStateStr = "ProbeFailed"
        case 11: fallbackStateStr = "StaleGeneration"
        default: fallbackStateStr = "Unknown (\(snapshot.busManager.irmFallbackState))"
        }
        r.row("Fallback State", fallbackStateStr)

        let fallbackActionStr: String
        switch snapshot.busManager.irmFallbackPlannedAction {
        case 0: fallbackActionStr = "None"
        case 1: fallbackActionStr = "DiagnosticsOnly (M4 report)"
        case 2: fallbackActionStr = "LocalRootEnableCycleMasterRequired"
        case 3: fallbackActionStr = "RemoteRootCmstrRequired"
        case 4: fallbackActionStr = "RootSelectionRequired"
        case 5: fallbackActionStr = "GapPolicyRequired"
        case 6: fallbackActionStr = "BMAlreadyExists"
        case 7: fallbackActionStr = "CycleStartAlreadyObserved"
        case 8: fallbackActionStr = "SuppressedByRolePolicy"
        case 9: fallbackActionStr = "SuppressedByActivityLevel"
        case 10: fallbackActionStr = "SuppressedByTopology"
        default: fallbackActionStr = "Unknown (\(snapshot.busManager.irmFallbackPlannedAction))"
        }
        r.row("Planned Action", fallbackActionStr)

        let probeStatusStr: String
        switch snapshot.busManager.irmFallbackProbeStatus {
        case 0: probeStatusStr = "NotAttempted"
        case 1: probeStatusStr = "Success"
        case 2: probeStatusStr = "InvalidUpperBits"
        case 3: probeStatusStr = "HardwareUnavailable"
        case 4: probeStatusStr = "Timeout"
        default: probeStatusStr = "Unknown (\(snapshot.busManager.irmFallbackProbeStatus))"
        }
        r.row("BUS_MANAGER_ID Probe Status", probeStatusStr)
        r.row("Probed BUS_MANAGER_ID Raw", DiagFormat.hex32(snapshot.busManager.irmFallbackRawBusManagerId))

        r.row("Annex H Fallback Gate", snapshot.busManager.irmFallbackAnnexHGateOpen != 0 ? "Open" : "Closed")
        if snapshot.busManager.irmFallbackAnnexHGateOpen == 0 {
            r.row("Fallback Allowed In", "\(snapshot.busManager.irmFallbackRemainingMs) ms")
        }

        r.row("Mutation Performed", "No")

        let activityStr: String
        switch snapshot.busManager.fullBMActivityLevel {
        // Order must match FullBMActivityLevel in ASFWDriver/Common/CSRSpace.hpp.
        case 0: activityStr = "ObserveOnly (BM-passive, IRM-visible)"
        case 1: activityStr = "ElectionOnly"
        case 2: activityStr = "CyclePolicyAllowed"
        case 3: activityStr = "GapPolicyAllowed"
        case 4: activityStr = "ForceRootAllowed"
        case 5: activityStr = "RemoteCmstrAllowed"
        default: activityStr = "Unknown (\(snapshot.busManager.fullBMActivityLevel))"
        }
        r.row("Full BM Activity Level", activityStr)
    }

    // --- Milestone 5: Cycle Master Policy ---
    static func appendCycleMaster(_ r: DiagnosticsReport,
                                  _ snapshot: ASFWDiagnosticsSnapshot) {
        r.title("Cycle Master Policy (Milestone 5)")

        let cycleDecisionStr: String
        switch snapshot.busManager.cyclePolicyDecision {
        case 0: cycleDecisionStr = "None"
        case 1: cycleDecisionStr = "SuppressedByRoleMode"
        case 2: cycleDecisionStr = "SuppressedByActivityLevel"
        case 3: cycleDecisionStr = "SuppressedByTopology"
        case 4: cycleDecisionStr = "SuppressedByGeneration"
        case 5: cycleDecisionStr = "SuppressedNotBMOrFallbackIRM"
        case 6: cycleDecisionStr = "AlreadySatisfiedCycleStartObserved"
        case 7: cycleDecisionStr = "AlreadySatisfiedLocalCycleMasterEnabled"
        case 8: cycleDecisionStr = "LocalCycleMasterClearNotRoot"
        case 9: cycleDecisionStr = "DeferRootSelfIDUnknown"
        case 10: cycleDecisionStr = "DeferLocalSelfIDUnknown"
        case 11: cycleDecisionStr = "LocalRootEnableCycleMaster"
        case 12: cycleDecisionStr = "RemoteRootSetCmstr"
        case 13: cycleDecisionStr = "RootSelectionRequired"
        case 14: cycleDecisionStr = "FailedHardwareUnavailable"
        case 15: cycleDecisionStr = "FailedAsyncSubmit"
        case 16: cycleDecisionStr = "FailedGenerationStale"
        case 17: cycleDecisionStr = "DeferRootBibCmcUnknown"
        default: cycleDecisionStr = "Unknown (\(snapshot.busManager.cyclePolicyDecision))"
        }
        r.row("Cycle Decision", cycleDecisionStr)

        let cycleActionStr: String
        switch snapshot.busManager.cyclePolicyAction {
        case 0: cycleActionStr = "None"
        case 1: cycleActionStr = "EnableLocalCycleMaster"
        case 2: cycleActionStr = "ClearLocalCycleMaster"
        case 3: cycleActionStr = "WriteRemoteStateSetCmstr"
        case 4: cycleActionStr = "ReportRootSelectionRequired"
        default: cycleActionStr = "Unknown (\(snapshot.busManager.cyclePolicyAction))"
        }
        r.row("Cycle Action", cycleActionStr)

        r.row("Cycle Target Node", snapshot.busManager.cyclePolicyTargetNode == 0x3F ? "none" : "node \(snapshot.busManager.cyclePolicyTargetNode)")
        r.row("Local Master Before", snapshot.busManager.cyclePolicyLocalLowLevelMasterBefore != 0 ? "Yes" : "No")
        r.row("Local Master After", snapshot.busManager.cyclePolicyLocalLowLevelMasterAfter != 0 ? "Yes" : "No")
        r.row("Remote CMSTR In Flight", snapshot.busManager.cyclePolicyRemoteCmstrInFlight != 0 ? "Yes" : "No")
        if snapshot.busManager.cyclePolicyRemoteSubmitCount > 0 ||
            snapshot.busManager.cyclePolicyRemoteCmstrInFlight != 0 {
            let statusStr: String
            if snapshot.busManager.cyclePolicyRemoteCmstrInFlight != 0 {
                statusStr = "in flight"
            } else {
                switch snapshot.busManager.cyclePolicyRemoteCmstrStatus {
                case 0: statusStr = "success"
                case 1: statusStr = "timeout"
                case 2: statusStr = "short_read"
                case 3: statusStr = "busy_retry_exhausted"
                case 4: statusStr = "aborted"
                case 5: statusStr = "hardware_error"
                case 6: statusStr = "lock_compare_fail"
                case 7: statusStr = "stale_generation"
                default: statusStr = "unknown (\(snapshot.busManager.cyclePolicyRemoteCmstrStatus))"
                }
            }
            r.row("Remote CMSTR Status", statusStr)
        }
        r.row("Local Enable Count", snapshot.busManager.cyclePolicyLocalEnableCount)
        r.row("Local Clear Count", snapshot.busManager.cyclePolicyLocalClearCount)
        r.row("Remote Submit Count", snapshot.busManager.cyclePolicyRemoteSubmitCount)

        let cycleScopeStr: String
        switch snapshot.busManager.fullBMActivityLevel {
        case 0, 1: cycleScopeStr = "Suppressed until CyclePolicyAllowed."
        case 2: cycleScopeStr = "Active. Root/gap policy disabled."
        case 3: cycleScopeStr = "Active. Gap policy may run separately; root forcing disabled."
        default: cycleScopeStr = "Active. Root/gap policy may run separately."
        }
        r.row("Cycle Master Policy", cycleScopeStr)
    }

    // --- Milestone 6: Root Selection / Force-Root Policy ---
    static func appendRootSelection(_ r: DiagnosticsReport,
                                    _ snapshot: ASFWDiagnosticsSnapshot) {
        r.title("Root Selection / Force-Root Policy (Milestone 6)")

        let rootDecisionStr: String
        switch snapshot.busManager.rootSelectionDecision {
        case 0: rootDecisionStr = "None"
        case 1: rootDecisionStr = "SuppressedByRoleMode"
        case 2: rootDecisionStr = "SuppressedByActivityLevel"
        case 3: rootDecisionStr = "SuppressedByTopology"
        case 4: rootDecisionStr = "SuppressedNotBMOrFallbackIRM"
        case 5: rootDecisionStr = "SuppressedCycleAlreadyObserved"
        case 6: rootDecisionStr = "SuppressedRootAlreadySuitable"
        case 7: rootDecisionStr = "DeferredRootSelfIDEvidenceIncomplete"
        case 8: rootDecisionStr = "DeferredCandidateEvidenceIncomplete"
        case 9: rootDecisionStr = "SelectLocalRoot"
        case 10: rootDecisionStr = "SelectRemoteRoot"
        case 11: rootDecisionStr = "FailedNoCandidate"
        case 12: rootDecisionStr = "FailedRetryLimit"
        case 13: rootDecisionStr = "FailedGenerationStale"
        case 14: rootDecisionStr = "FailedExecutorUnavailable"
        default: rootDecisionStr = "Unknown (\(snapshot.busManager.rootSelectionDecision))"
        }
        r.row("Decision", rootDecisionStr)

        let rootActionStr: String
        switch snapshot.busManager.rootSelectionAction {
        case 0: rootActionStr = "None"
        case 1: rootActionStr = "ForceRootAndShortReset"
        case 2: rootActionStr = "ForceRootAndLongReset"
        case 3: rootActionStr = "ReportOnly"
        default: rootActionStr = "Unknown (\(snapshot.busManager.rootSelectionAction))"
        }
        r.row("Action", rootActionStr)

        r.row("Previous Root", snapshot.busManager.rootSelectionPreviousRoot == 0x3F ? "none" : "node \(snapshot.busManager.rootSelectionPreviousRoot)")
        r.row("Selected Root", snapshot.busManager.rootSelectionSelectedRoot == 0x3F ? "none" : "node \(snapshot.busManager.rootSelectionSelectedRoot)")

        let rootReasonStr: String
        switch snapshot.busManager.rootSelectionDecision {
        case 6: rootReasonStr = "Current root already Self-ID contender/link-active"
        case 10: rootReasonStr = "Remote Self-ID contender selected"
        case 11: rootReasonStr = "No Self-ID contender candidates found"
        case 12: rootReasonStr = "Reset attempt limit reached for this topology"
        default: rootReasonStr = "none"
        }
        r.row("Reason", rootReasonStr)

        r.row("Attempts This Topology", "\(snapshot.busManager.rootSelectionAttemptsThisTopology) / 5")
        r.row("Total Attempts", snapshot.busManager.rootSelectionTotalAttempts)
        r.row("Reset Requested", snapshot.busManager.rootSelectionResetRequested != 0 ? "Yes" : "No")
        r.row("Current Gap Count", snapshot.busManager.rootSelectionCurrentGap)
        r.row("Requested Gap Count", "preserve current gap")
        r.row("Gap Optimization", "disabled; M7 owns optimization")
    }

    // --- Milestone 7: Gap Count Policy ---
    static func appendGapCount(_ r: DiagnosticsReport,
                               _ snapshot: ASFWDiagnosticsSnapshot) {
        r.title("Gap Count Policy (Milestone 7)")

        let gapDecisionStr: String
        switch snapshot.busManager.gapPolicyDecision {
        case 0: gapDecisionStr = "None"
        case 1: gapDecisionStr = "SuppressedByRoleMode"
        case 2: gapDecisionStr = "SuppressedByActivityLevel"
        case 3: gapDecisionStr = "SuppressedByTopology"
        case 4: gapDecisionStr = "SuppressedNotBMOrFallbackIRM"
        case 5: gapDecisionStr = "SuppressedSingleNodeBus"
        case 6: gapDecisionStr = "DeferMaxHopsUnavailable"
        case 7: gapDecisionStr = "DeferBetaRepeaterUnknown"
        case 8: gapDecisionStr = "AlreadyOptimal"
        case 9: gapDecisionStr = "GapMismatchRequiresLongReset"
        case 10: gapDecisionStr = "GapOptimizationRequired"
        case 11: gapDecisionStr = "FailedRetryLimit"
        case 12: gapDecisionStr = "FailedExecutorUnavailable"
        case 13: gapDecisionStr = "FailedGenerationStale"
        default: gapDecisionStr = "Unknown (\(snapshot.busManager.gapPolicyDecision))"
        }
        r.row("Decision", gapDecisionStr)

        let gapActionStr: String
        switch snapshot.busManager.gapPolicyAction {
        case 0: gapActionStr = "None"
        case 1: gapActionStr = "ReportOnly"
        case 2: gapActionStr = "ForceRootWithGapAndShortReset"
        case 3: gapActionStr = "ForceRootWithGapAndLongReset"
        case 4: gapActionStr = "GapOnlyShortReset"
        case 5: gapActionStr = "GapOnlyLongReset"
        default: gapActionStr = "Unknown (\(snapshot.busManager.gapPolicyAction))"
        }
        r.row("Action", gapActionStr)

        r.row("Current Gap", snapshot.busManager.gapPolicyCurrentGap)
        r.row("Expected Gap", snapshot.busManager.gapPolicyExpectedGap)
        r.row("Requested Gap", snapshot.busManager.gapPolicyRequestedGap)

        let gapSourceStr: String
        switch snapshot.busManager.gapPolicyComputationSource {
        case 0: gapSourceStr = "None"
        case 1: gapSourceStr = "1394a table (max hops)"
        case 2: gapSourceStr = "Default safe 63"
        case 3: gapSourceStr = "Existing gap preserved"
        default: gapSourceStr = "Unknown"
        }
        r.row("Computation Source", gapSourceStr)

        r.row("Max Hops From Root", snapshot.busManager.gapPolicyMaxHopsKnown != 0 ? "\(snapshot.busManager.gapPolicyMaxHops)" : "unknown")
        r.row("Gap Count Consistent", snapshot.busManager.gapPolicyGapConsistent != 0 ? "Yes" : "No")

        let betaStr: String
        if snapshot.busManager.gapPolicyBetaKnown == 0 {
            betaStr = "Unknown"
        } else {
            betaStr = snapshot.busManager.gapPolicyBetaPresent != 0 ? "Known Yes" : "Known No"
        }
        r.row("Beta Repeaters", betaStr)

        r.row("Target Root", "node \(snapshot.busManager.gapPolicyTargetRoot)")
        r.row("Combined with M6", snapshot.busManager.gapPolicyCombinedWithRootSelection != 0 ? "Yes" : "No")
        r.row("Attempts This Topology", "\(snapshot.busManager.gapPolicyAttemptsThisTopology) / 5")
        r.row("Reset Requested", snapshot.busManager.gapPolicyResetRequested != 0 ? "Yes" : "No")
    }

    // --- Milestone 8: Power / Link-On Policy ---
    static func appendPower(_ r: DiagnosticsReport,
                            _ snapshot: ASFWDiagnosticsSnapshot) {
        r.title("Power / Link-On Policy (Milestone 8)")

        let powerDecisionStr: String
        switch snapshot.busManager.powerPolicyDecision {
        case 0: powerDecisionStr = "None"
        case 1: powerDecisionStr = "SuppressedByRoleMode"
        case 2: powerDecisionStr = "SuppressedByPolicyLevel"
        case 3: powerDecisionStr = "SuppressedByTopology"
        case 4: powerDecisionStr = "SuppressedNotBMOrFallbackIRM"
        case 5: powerDecisionStr = "NoEligibleNodes"
        case 6: powerDecisionStr = "DeferredPowerBudgetUnknown"
        case 7: powerDecisionStr = "DeferredInsufficientPower"
        case 8: powerDecisionStr = "DeferredNodeEvidenceIncomplete"
        case 9: powerDecisionStr = "LinkOnRequired"
        case 10: powerDecisionStr = "LinkOnAlreadyAttemptedThisGeneration"
        case 11: powerDecisionStr = "FailedRetryLimit"
        case 12: powerDecisionStr = "FailedExecutorUnavailable"
        case 13: powerDecisionStr = "FailedGenerationStale"
        default: powerDecisionStr = "Unknown (\(snapshot.busManager.powerPolicyDecision))"
        }
        r.row("Decision", powerDecisionStr)

        let powerActionStr: String
        switch snapshot.busManager.powerPolicyAction {
        case 0: powerActionStr = "None"
        case 1: powerActionStr = "ReportOnly"
        case 2: powerActionStr = "SendLinkOnPackets"
        default: powerActionStr = "Unknown (\(snapshot.busManager.powerPolicyAction))"
        }
        r.row("Action", powerActionStr)

        let budgetStr: String
        switch snapshot.busManager.powerBudgetStatus {
        case 0: budgetStr = "Unknown"
        case 1: budgetStr = "Sufficient"
        case 2: budgetStr = "Insufficient"
        default: budgetStr = "Invalid (\(snapshot.busManager.powerBudgetStatus))"
        }
        r.row("Power Budget", budgetStr)

        r.row("Eligible Nodes", snapshot.busManager.powerEligibleNodeCount)

        if snapshot.busManager.powerTargetNodeCount > 0 {
            // Safe access to fixed-size array powerTargetNodes[16] via mirroring or manual indexing
            // Since we're in Swift, we treat it as a tuple or use Reflection if needed.
            // For now, let's assume we can access them if the ABI allows.
            // Actually, we'll just report the count and first few if possible.
            r.row("Target Node Count", snapshot.busManager.powerTargetNodeCount)
        }

        r.row("Attempts This Generation", "\(snapshot.busManager.linkOnAttemptsThisGeneration) / 1")
        r.row("Submitted", snapshot.busManager.linkOnSubmittedCount)
        r.row("Succeeded", snapshot.busManager.linkOnSuccessCount)
        r.row("Failed", snapshot.busManager.linkOnFailureCount)
    }

    // --- Milestone 9: CSR Compliance / Maps ---
    static func appendCSRCompliance(_ r: DiagnosticsReport,
                                    _ snapshot: ASFWDiagnosticsSnapshot) {
        r.title("CSR Compliance / Maps (Milestone 9)")

        let topoStatusStr: String
        switch snapshot.busManager.topologyMapPublishStatus {
        case 1: topoStatusStr = "Valid"
        case 2: topoStatusStr = "ZeroLength (topology error)"
        case 3: topoStatusStr = "StaleGeneration"
        default: topoStatusStr = "Invalid"
        }
        r.row("TOPOLOGY_MAP Owner", "Software")
        r.row("TOPOLOGY_MAP State", topoStatusStr)
        r.row("TOPOLOGY_MAP Generation", snapshot.busManager.topologyMapGeneration)
        r.row("TOPOLOGY_MAP Self-IDs", snapshot.busManager.topologyMapSelfIdCount)

        let speedStatusStr: String
        switch snapshot.busManager.speedMapStatus {
        case 1: speedStatusStr = "Valid"
        case 2: speedStatusStr = "ConservativeFallback"
        case 3: speedStatusStr = "UnsupportedBetaPath"
        default: speedStatusStr = "Invalid"
        }
        r.row("SPEED_MAP Owner", "Software (legacy; obsolete in IEEE 1394-2008)")
        r.row("SPEED_MAP State", speedStatusStr)
        r.row("SPEED_MAP Generation", snapshot.busManager.speedMapGeneration)
        r.row("SPEED_MAP Nodes", snapshot.busManager.speedMapNodeCount)
        r.row("SPEED_MAP Encoding", "\(snapshot.busManager.speedMapEncodedQuadlets) quadlets")

        r.row("Core IRM CSRs Owner", "OHCI hardware (remote telemetry not observable)")
        r.row("Unexpected SW Hits", snapshot.busManager.unexpectedResourceCsrSoftwareCount)
        r.row("CSR Verdict Scope", "ownership + TOPOLOGY_MAP; SPEED_MAP is legacy/obsolete")
        let csrVerdictStr: String
        switch snapshot.busManager.csrContractVerdict {
        case 1: csrVerdictStr = "OK"
        case 2: csrVerdictStr = "Verifier unavailable"
        default: csrVerdictStr = "Mismatch"
        }
        r.row("CSR Contract Verdict", csrVerdictStr)
        if snapshot.busManager.speedMapStatus == 0 ||
            snapshot.busManager.speedMapGeneration != snapshot.busManager.topologyMapGeneration {
            r.row("SPEED_MAP Legacy Health", "stale/invalid (not fatal)")
        } else {
            r.row("SPEED_MAP Legacy Health", "fresh")
        }
        if snapshot.busManager.csrContractVerdict == 0 {
            if snapshot.busManager.topologyMapPublishStatus != 1 || snapshot.busManager.topologyMapGeneration == 0 {
                r.row("Mismatch Reason", "TOPOLOGY_MAP invalid or not published")
            }
            if snapshot.busManager.unexpectedResourceCsrSoftwareCount != 0 {
                r.row("Mismatch Reason", "OHCI-owned CSR reached software responder")
            }
            r.row("SW Answered HW-owned", snapshot.busManager.csrSoftwareAnsweredHardwareOwned)
            r.row("HW-owned SW Hits", snapshot.busManager.csrHardwareOwnedSoftwareHits)
            r.row("Unsupported Accesses", snapshot.busManager.csrUnsupportedAccesses)
        }
    }

    // --- Post-Reset Timing (IEEE 1394-2008 §8.x) ---
    // Generation-scoped gates anchored to Self-ID completion. Reporting only:
    // the driver takes no bus action from these gates in this milestone, so an
    // Open BM gate with a "Not a candidate" class still means the local node
    // will not contend (role policy suppresses it).
    static func appendPostResetTiming(_ r: DiagnosticsReport,
                                      _ snapshot: ASFWDiagnosticsSnapshot) {
        let prt = snapshot.postResetTiming
        // TimingGateState (ASFWDriver/Bus/Timing/PostResetTiming.hpp).
        func gateStateName(_ v: UInt32) -> String {
            switch v {
            case 0: return "Unknown"
            case 1: return "Closed"
            case 2: return "Open"
            case 3: return "Expired (old generation)"
            case 4: return "Suppressed (role policy)"
            case 5: return "Suppressed (topology)"
            default: return "Unknown (\(v))"
            }
        }
        // A delayed gate renders as "Open" or "Closed / opens in N ms".
        func gateLine(_ state: UInt32, _ remainingNs: UInt64) -> String {
            switch state {
            case 2: return "Open"
            case 1:
                if prt.selfIdComplete == 0 { return "Closed (awaiting Self-ID)" }
                return String(format: "Closed / opens in %.1f ms", Double(remainingNs) / 1_000_000.0)
            default: return gateStateName(state)
            }
        }
        // BMCandidateClass (ASFWDriver/Bus/Timing/PostResetTiming.hpp).
        func candidateClassName(_ v: UInt32) -> String {
            switch v {
            case 0: return "Not a candidate"
            case 1: return "Incumbent"
            case 2: return "Non-incumbent"
            default: return "Unknown (\(v))"
            }
        }
        r.title("Post-Reset Timing")
        r.row("Self-ID Complete", prt.selfIdComplete != 0 ? "Yes" : "No")
        r.row("Generation", prt.generation)
        r.row("Self-ID Age", String(format: "%.3f ms", Double(prt.ageSinceSelfIdNs) / 1_000_000.0))
        r.row("BM Incumbent Gate",
              prt.incumbentBMGate == 2 ? "Open" : gateStateName(prt.incumbentBMGate))
        r.row("BM Non-Incumbent Gate", gateLine(prt.nonIncumbentBMGate, prt.nonIncumbentBMRemainingNs))
        r.row("IRM Fallback Gate", gateLine(prt.irmFallbackGate, prt.irmFallbackRemainingNs))
        r.row("New ISO Allocation Gate", gateLine(prt.newIsoAllocationGate, prt.newIsoAllocationRemainingNs))
        r.row("BM Candidate Class", candidateClassName(prt.bmCandidateClass))
        r.row("Stale Timer Firings", prt.staleTimerFirings)
        r.row("Suppressed By Generation", prt.suppressedByGeneration)
        r.row("Suppressed By Role Policy", prt.suppressedByRolePolicy)
    }
}
