#include "BusResetCoordinator.hpp"

#ifdef ASFW_HOST_TEST
#include <chrono>
#include <thread>
#else
#include <DriverKit/IOLib.h>
#endif

#include "../Async/AsyncSubsystem.hpp"
#include "../ConfigROM/ConfigROMStager.hpp"
#include "../ConfigROM/ROMScanner.hpp"
#include "../Hardware/OHCIConstants.hpp"
#include "BusManager.hpp"
#include "HardwareInterface.hpp"
#include "InterruptManager.hpp"
#include "Logging.hpp"
#include "SelfIDCapture.hpp"
#include "TopologyManager.hpp"

namespace ASFW::Driver {

void BusResetCoordinator::A_MaskBusReset() {
    // Route through InterruptManager to keep software shadow in sync
    if ((interruptManager_ == nullptr) || (hardware_ == nullptr)) {
        return;
    }
    interruptManager_->MaskInterrupts(hardware_, IntEventBits::kBusReset);
    ASFW_LOG(BusReset, "[Action] Masked busReset interrupt");
    busResetMasked_ = true;

    // OHCI §3.1.1.3 + §7.2.3.1:
    // Hardware automatically clears ContextControl.active for AT contexts
    // when a bus reset occurs. This temporary software mask only prevents
    // overlapping busReset edges during our FSM-controlled cleanup.
    // Not required by spec but aligns with Linux post-reset delay behavior.
    //
    // IMPORTANT: Do not mask other interrupt bits here — hardware guarantees
    // isolation between busReset and unrelated DMA contexts.
}

void BusResetCoordinator::A_UnmaskBusReset() {
    // Route through InterruptManager to keep software shadow in sync
    if ((interruptManager_ == nullptr) || (hardware_ == nullptr)) {
        return;
    }
    interruptManager_->UnmaskInterrupts(hardware_, IntEventBits::kBusReset);
    ASFW_LOG(BusReset, "[Action] Unmasked busReset (with masterIntEnable ensured)");
    busResetMasked_ = false;
}

void BusResetCoordinator::ForceUnmaskBusResetIfNeeded() {
    if (!busResetMasked_) {
        return;
    }

    if ((interruptManager_ == nullptr) || (hardware_ == nullptr)) {
        ASFW_LOG(
            BusReset,
            "⚠️  busReset interrupt remained masked but cannot unmask (interruptMgr=%p hardware=%p)",
            interruptManager_, hardware_);
        return;
    }

    ASFW_LOG(BusReset,
             "[Action] Forcing busReset interrupt unmask to re-enable future bus reset detection");
    interruptManager_->UnmaskInterrupts(hardware_, IntEventBits::kBusReset);
    busResetMasked_ = false;
}

void BusResetCoordinator::A_ClearSelfID2Stale() {
    if (hardware_ == nullptr) {
        return;
    }
    hardware_->Write(Register32::kIntEventClear, IntEventBits::kSelfIDComplete2);
    ASFW_LOG(BusReset, "[Action] Cleared stale selfIDComplete2");
}

void BusResetCoordinator::A_ArmSelfIDBuffer() {
    if ((selfIdCapture_ == nullptr) || (hardware_ == nullptr)) {
        return;
    }
    kern_return_t ret = selfIdCapture_->Arm(*hardware_);
    if (ret != kIOReturnSuccess) {
        ASFW_LOG(BusReset, "[Action] Failed to arm Self-ID buffer: 0x%x", ret);
    }
}

void BusResetCoordinator::A_AckSelfIDPair() {
    if (hardware_ == nullptr) {
        return;
    }

    // Clear sticky Self-ID interrupt bits now that we've consumed the buffer
    // Per OHCI §6.1.1: selfIDComplete and selfIDComplete2 are sticky status bits
    // that must be explicitly cleared to prevent continuous IRQ assertion
    uint32_t toClear = 0;
    if (selfIDComplete1_) {
        toClear |= IntEventBits::kSelfIDComplete;
    }
    if (selfIDComplete2_) {
        toClear |= IntEventBits::kSelfIDComplete2;
    }

    if (toClear != 0U) {
        hardware_->WriteAndFlush(Register32::kIntEventClear, toClear);
        ASFW_LOG(BusReset, "[Action] Acked Self-ID interrupts: clear=0x%08x", toClear);
    } else {
        ASFW_LOG(BusReset, "[Action] AckSelfIDPair skipped (no bits set)");
    }

    // Reset latched flags so next cycle can detect fresh Self-ID pair
    selfIDComplete1_ = false;
    selfIDComplete2_ = false;
}

void BusResetCoordinator::A_StopFlushAT() {
    if (asyncSubsystem_ == nullptr) {
        return;
    }

    const uint8_t nextGen =
        (lastGeneration_.value == 0xFFU) ? 0 : static_cast<uint8_t>(lastGeneration_.value + 1U);
    asyncSubsystem_->OnBusResetBegin(nextGen);

    ASFW_LOG(BusReset, "[Action] Stopping AT contexts");
    asyncSubsystem_->StopATContextsOnly();

    ASFW_LOG(BusReset, "[Action] Flushing AT context descriptors");
    asyncSubsystem_->FlushATContexts();

    ASFW_LOG(BusReset, "[Action] AT contexts stop+flush complete");
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void BusResetCoordinator::A_DecodeSelfID() {
    if ((selfIdCapture_ == nullptr) || (hardware_ == nullptr)) {
        return;
    }

    const uint32_t countReg = hardware_->Read(Register32::kSelfIDCount);
    pendingSelfIDCountReg_ = countReg;

    const uint32_t nodeIDReg = hardware_->Read(Register32::kNodeID);
    const bool iDValid = (nodeIDReg & 0x80000000U) != 0U;

    if (!iDValid) {
        ASFW_LOG(BusReset, "  ⚠️ iDValid=0 indicates Self-ID phase not complete");
    }

    auto result = selfIdCapture_->Decode(countReg, *hardware_);
    lastSelfId_ = result;

    if (result && result->valid) {
        lastGeneration_ = Discovery::Generation{result->generation};
        ASFW_LOG(BusReset, "[Action] Self-ID decoded: gen=%u, %zu quads", result->generation,
                 result->quads.size());
        if (asyncSubsystem_ != nullptr) {
            asyncSubsystem_->ConfirmBusGeneration(static_cast<uint8_t>(result->generation & 0xFFU));
        }

        if (result->quads.size() > 1) {
            constexpr uint32_t kGapMask = 0x003F0000U;
            const uint32_t localSelfId = result->quads[1];
            const uint8_t gapCount = static_cast<uint8_t>((localSelfId & kGapMask) >> 16);

            if (gapCount == 0) {
                ASFW_LOG(BusReset, "⚠️ Local gap count zero – sending reactive PHY fix");

                BusManager::PhyConfigCommand fix{};
                fix.gapCount = std::uint8_t{0x3F};

                if (hardware_->SendPhyConfig(fix.gapCount, fix.forceRootNodeID,
                                             "GapCountZeroFix")) {
                    ASFW_LOG(BusReset,
                             "Reactive gap fix PHY packet sent; initiating short bus reset");
                    const bool resetOk = hardware_->InitiateBusReset(true);
                    if (!resetOk) {
                        ASFW_LOG_ERROR(BusReset, "Reactive short reset failed to start");
                    }
                    return;
                }

                ASFW_LOG_ERROR(BusReset, "Reactive gap fix PHY send failed");
            }
        }
    } else {
        ASFW_LOG(BusReset, "[Action] Self-ID decode failed");
        if (result && !result->crcError && !result->timedOut) {
            metrics_.lastFailureReason = "Self-ID generation mismatch";
        } else if (result && result->crcError) {
            metrics_.lastFailureReason = "Self-ID CRC error";
        } else if (result && result->timedOut) {
            metrics_.lastFailureReason = "Self-ID timeout";
        } else {
            metrics_.lastFailureReason = "Self-ID decode failed";
        }
    }
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void BusResetCoordinator::A_BuildTopology() {
    if ((topologyManager_ == nullptr) || (selfIdCapture_ == nullptr) || (hardware_ == nullptr)) {
        return;
    }

    if (!lastSelfId_.has_value() || !lastSelfId_->valid) {
        return;
    }

    const uint32_t nodeIDReg = hardware_->Read(Register32::kNodeID);
    const uint64_t timestamp = MonotonicNow();

    auto snapshot = topologyManager_->UpdateFromSelfID(*lastSelfId_, timestamp, nodeIDReg);

    if (snapshot.has_value()) {
        ASFW_LOG(BusReset,
                 "[Action] Topology built: gen=%u nodes=%u root=%{public}s IRM=%{public}s "
                 "local=%{public}s",
                 snapshot->generation, snapshot->nodeCount,
                 snapshot->rootNodeId.has_value() ? std::to_string(*snapshot->rootNodeId).c_str()
                                                  : "none",
                 snapshot->irmNodeId.has_value() ? std::to_string(*snapshot->irmNodeId).c_str()
                                                 : "none",
                 snapshot->localNodeId.has_value() ? std::to_string(*snapshot->localNodeId).c_str()
                                                   : "none");

        lastTopology_ = *snapshot;
    } else {
        ASFW_LOG(BusReset, "[Action] Topology build returned nullopt");
        lastTopology_ = std::nullopt;
    }
}

void BusResetCoordinator::A_RestoreConfigROM() {
    if ((configRomStager_ == nullptr) || (hardware_ == nullptr)) {
        return;
    }

    configRomStager_->RestoreHeaderAfterBusReset();

    const uint32_t busOpts = configRomStager_->ExpectedBusOptions();
    hardware_->WriteAndFlush(Register32::kBusOptions, busOpts);

    const uint32_t romHeader = configRomStager_->ExpectedHeader();
    hardware_->WriteAndFlush(Register32::kConfigROMHeader, romHeader);
    ASFW_LOG(BusReset, "[Action] ConfigROMheader written: 0x%08x", romHeader);
}

void BusResetCoordinator::A_ClearBusReset() {
    if (hardware_ == nullptr) {
        return;
    }
    hardware_->WriteAndFlush(Register32::kIntEventClear, IntEventBits::kBusReset);
    busResetClearTime_ = MonotonicNow();

    ASFW_LOG(BusReset, "[Action] busReset interrupt event cleared");
}

void BusResetCoordinator::A_EnableFilters() {
    if (hardware_ == nullptr) {
        return;
    }

    hardware_->Write(Register32::kAsReqFilterHiSet, kAsReqAcceptAllMask);
    filtersEnabled_ = true;
    ASFW_LOG(BusReset, "[Action] AsynchronousRequestFilter enabled");
}

void BusResetCoordinator::A_RearmAT() {
    if (asyncSubsystem_ == nullptr) {
        return;
    }
    asyncSubsystem_->RearmATContexts();
    atArmed_ = true;
    ASFW_LOG(BusReset, "[Action] AT contexts re-armed");
}

void BusResetCoordinator::A_MetricsLog() {
    const uint64_t now = MonotonicNow();
    const uint64_t completionTime = now;
    const uint64_t durationNs = completionTime - firstIrqTime_;
    // Read final NodeID to capture our bus position
    uint32_t finalNodeID = 0;
    bool nodeIDValid = false;
    if (hardware_ != nullptr) {
        finalNodeID = hardware_->Read(Register32::kNodeID);
        nodeIDValid = (finalNodeID & 0x80000000U) != 0U; // iDValid bit
    }

    // Extract generation from cached Self-ID decode result
    uint32_t generation = 0;
    if (lastSelfId_.has_value() && lastSelfId_->valid) {
        generation = lastSelfId_->generation;
    }

    const uint32_t nodeNumber = finalNodeID & 0x3FU;
    const uint32_t busNumber =
        (finalNodeID >> 6) & 0x3FFU; // Fixed: bits[15:6] per OHCI §5.11 Table 47
    const double durationMsDouble = static_cast<double>(durationNs) / 1'000'000.0;

    ASFW_LOG(BusReset,
             "Bus reset #%u complete: duration=%.2f ms gen=%u nodeID=0x%08x(bus=%u node=%u "
             "valid=%d) aborts=%u",
             metrics_.resetCount, durationMsDouble, generation, finalNodeID, busNumber, nodeNumber,
             nodeIDValid, metrics_.abortCount);

#if ASFW_DEBUG_BUS_RESET
    ASFW_LOG_BUSRESET_DETAIL(
        "  first_irq=%llu selfid1=%llu selfid2=%llu cleared=%llu completed=%llu", firstIrqTime_,
        selfIDComplete1Time_, selfIDComplete2Time_, busResetClearTime_, completionTime);
#endif

    if (metrics_.lastFailureReason.has_value()) {
        ASFW_LOG(BusReset, "  Last failure cleared: %{public}s",
                 metrics_.lastFailureReason->c_str());
    }

    // Update BusResetMetrics structure for DriverKit status queries
    metrics_.lastResetStart = firstIrqTime_;
    metrics_.lastResetCompletion = completionTime;
    metrics_.lastFailureReason.reset(); // Clear stale failure text on success
}

void BusResetCoordinator::A_SendGlobalResumeIfNeeded() {
    if (hardware_ == nullptr) {
        return;
    }
    if (!lastTopology_.has_value() || !lastTopology_->localNodeId.has_value()) {
        return;
    }

    const uint32_t generation = lastTopology_->generation;
    if (lastResumeGeneration_ == generation) {
        return;
    }

    const uint8_t localNode = *lastTopology_->localNodeId;
    if (hardware_->SendPhyGlobalResume(localNode)) {
        lastResumeGeneration_ = generation;
        ASFW_LOG(BusReset, "PHY Global Resume dispatched (node=%u generation=%u)", localNode,
                 generation);
    } else {
        ASFW_LOG_ERROR(BusReset, "PHY Global Resume failed to send (node=%u generation=%u)",
                       localNode, generation);
    }
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void BusResetCoordinator::StageDelayedPhyPacket(const BusManager::PhyConfigCommand& command,
                                                const char* reason) {
    const bool isDelegate = (reason != nullptr) && (std::strcmp(reason, "AssignCycleMaster") == 0);

    // Check persistent suppression first (Linux pattern: prevents infinite loops)
    if (isDelegate && delegateSuppressed_) {
        ASFW_LOG(BusReset, "Root delegation suppressed (exceeded retry limit of %u)",
                 kMaxDelegateRetries);
        return;
    }

    if (pendingPhyCommand_.has_value()) {
        ASFW_LOG(BusReset,
                 "Deferred PHY packet already staged (existing reason=%{public}s) - ignoring new "
                 "request",
                 pendingPhyReason_.c_str());
        return;
    }

    if (isDelegate && delegateAttemptActive_) {
        ASFW_LOG(BusReset,
                 "Skipping new AssignCycleMaster request - previous delegation still in flight "
                 "(target=%u)",
                 delegateTarget_);
        return;
    }

    if (isDelegate && command.forceRootNodeID.has_value()) {
        // Check if target changed (topology change)
        const uint8_t newTarget = *command.forceRootNodeID;
        if (newTarget != delegateTarget_) {
            // Target changed - reset retry counter
            delegateRetryCount_ = 0;
            delegateTarget_ = newTarget;
            ASFW_LOG(BusReset, "Delegation target changed to node %u - retry counter reset",
                     newTarget);
        }

        // Increment retry counter (Linux pattern)
        delegateRetryCount_++;

        // Check retry limit (Linux: max 5 attempts)
        if (delegateRetryCount_ > kMaxDelegateRetries) {
            delegateSuppressed_ = true;
            ASFW_LOG(BusReset,
                     "Root delegation to node %u failed after %u attempts - suppressing further "
                     "attempts",
                     delegateTarget_, kMaxDelegateRetries);
            return;
        }

        delegateAttemptActive_ = true;
        ASFW_LOG(BusReset, "Root delegation attempt %u/%u to node %u", delegateRetryCount_,
                 kMaxDelegateRetries, delegateTarget_);
    }

    pendingPhyCommand_ = command;
    pendingPhyReason_ = (reason != nullptr) ? reason : "unspecified";
    pendingManagedReset_ = true;

    const std::string rootStr =
        command.forceRootNodeID.has_value() ? std::to_string(*command.forceRootNodeID) : "none";
    const std::string gapStr =
        command.gapCount.has_value() ? std::to_string(*command.gapCount) : "none";
    ASFW_LOG(BusReset, "Staged PHY packet (reason=%{public}s root=%{public}s gap=%{public}s)",
             pendingPhyReason_.c_str(), rootStr.c_str(), gapStr.c_str());
}

bool BusResetCoordinator::DispatchPendingPhyPacket() {
    if (!pendingPhyCommand_.has_value() || (hardware_ == nullptr)) {
        return false;
    }

    const auto cmd = *pendingPhyCommand_;
    const std::string rootStr =
        cmd.forceRootNodeID.has_value() ? std::to_string(*cmd.forceRootNodeID) : "none";
    const std::string gapStr = cmd.gapCount.has_value() ? std::to_string(*cmd.gapCount) : "none";

    ASFW_LOG(BusReset,
             "Dispatching delayed PHY packet (reason=%{public}s root=%{public}s gap=%{public}s)",
             pendingPhyReason_.c_str(), rootStr.c_str(), gapStr.c_str());

    if (cmd.setContender.has_value()) {
        ASFW_LOG(BusReset, "Applying Contender bit update: %d", *cmd.setContender);
        hardware_->SetContender(*cmd.setContender);
    }

    if (!hardware_->SendPhyConfig(cmd.gapCount, cmd.forceRootNodeID,
                                  "BusResetCoordinator::DispatchPendingPhyPacket")) {
        ASFW_LOG(BusReset, "⚠️  Failed to send staged PHY packet");
        if (std::strcmp(pendingPhyReason_.c_str(), "AssignCycleMaster") == 0) {
            delegateAttemptActive_ = false;
            delegateTarget_ = 0xFF;
            delegateRetryCount_ = 0;
            delegateSuppressed_ = false;
        }
        pendingManagedReset_ = false;
        pendingPhyCommand_.reset();
        pendingPhyReason_.clear();
        return false;
    }

    const bool resetOk = hardware_->InitiateBusReset(/*shortReset=*/false);
    if (!resetOk) {
        ASFW_LOG(BusReset, "⚠️  Failed to initiate bus reset after staged PHY packet");
        if (std::strcmp(pendingPhyReason_.c_str(), "AssignCycleMaster") == 0) {
            delegateAttemptActive_ = false;
            delegateTarget_ = 0xFF;
            delegateRetryCount_ = 0;
            delegateSuppressed_ = false;
        }
        pendingManagedReset_ = false;
        pendingPhyCommand_.reset();
        pendingPhyReason_.clear();
        return false;
    }

    pendingPhyCommand_.reset();
    pendingPhyReason_.clear();
    pendingManagedReset_ = false;
    return true;
}

void BusResetCoordinator::ScheduleDeferredRun(uint32_t delayMs, const char* reason) {
    if (workQueue_.get() == nullptr) {
        return;
    }

    bool expected = false;
    if (!deferredRunScheduled_.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        ASFW_LOG_BUSRESET_DETAIL("[FSM] Deferred run already scheduled (reason=%{public}s)",
                                 (reason != nullptr) ? reason : "unknown");
        return;
    }

    workQueue_->DispatchAsync(^{
#ifdef ASFW_HOST_TEST
      if (delayMs > 0) {
          std::this_thread::sleep_for(std::chrono::milliseconds(delayMs));
      }
#else
        if (delayMs > 0) {
            IOSleep(delayMs);
        }
#endif
      deferredRunScheduled_.store(false, std::memory_order_release);
      this->RunStateMachine();
    });
}

// Handle stray Self-ID interrupts that arrive outside normal reset flow
// This prevents infinite IRQ loops from sticky selfIDComplete/selfIDComplete2 bits
void BusResetCoordinator::HandleStraySelfID() {
    if ((hardware_ == nullptr) || (selfIdCapture_ == nullptr)) {
        ASFW_LOG(BusReset, "[FSM] HandleStraySelfID: missing dependencies (hw=%p selfId=%p)",
                 hardware_, selfIdCapture_);
        return;
    }

    // If NodeID.iDValid=1, treat as late completion and synthesize normal path
    if (G_NodeIDValid()) {
        ASFW_LOG(BusReset,
                 "[FSM] Stray Self-ID while Idle, NodeID valid → synthesize reset completion");
        A_DecodeSelfID();
        A_AckSelfIDPair(); // Clear sticky bits 15/16
        TransitionTo(State::QuiescingAT, "SYNTH: Self-ID complete while Idle");
        RunStateMachine(); // Continue FSM processing
        return;
    }

    // NodeID invalid - just ACK and ignore (late/spurious interrupt)
    ASFW_LOG(BusReset, "[FSM] Stray Self-ID while Idle, NodeID invalid → ack & ignore");
    A_AckSelfIDPair(); // Clear sticky bits 15/16, remain Idle
}

void BusResetCoordinator::EvaluateRootDelegation(const TopologySnapshot& topo) {
    if (!delegateAttemptActive_) {
        if (delegateSuppressed_ && topo.rootNodeId.has_value() && topo.localNodeId.has_value()) {
            const uint8_t currentRoot = *topo.rootNodeId;
            const uint8_t local = *topo.localNodeId;
            if (currentRoot != local) {
                delegateSuppressed_ = false;
                ASFW_LOG(BusReset, "Root delegation suppression cleared (local=%u currentRoot=%u)",
                         local, currentRoot);
            }
        }
        return;
    }

    if (!topo.rootNodeId.has_value()) {
        return;
    }

    const uint8_t currentRoot = *topo.rootNodeId;
    const uint8_t localNode = topo.localNodeId.value_or(0xFF);

    if ((delegateTarget_ != 0xFF && currentRoot == delegateTarget_) ||
        (localNode != 0xFF && currentRoot != localNode)) {
        ASFW_LOG(BusReset, "Root delegation succeeded (root=%u target=%u local=%u)", currentRoot,
                 delegateTarget_, localNode);
        delegateAttemptActive_ = false;
        delegateSuppressed_ = false;
        delegateTarget_ = 0xFF;
        delegateRetryCount_ = 0;
        return;
    }

    // Delegation attempt failed - let retry counter decide if we should suppress
    // Don't immediately suppress on first failure (Linux pattern: allows retries)
    ASFW_LOG(BusReset,
             "Root delegation to node %u failed (current root=%u local=%u) - retry attempt %u/%u",
             delegateTarget_, currentRoot, localNode, delegateRetryCount_, kMaxDelegateRetries);
    delegateAttemptActive_ = false;
    // Note: Do NOT set delegateSuppressed_ here - let StageDelayedPhyPacket handle retry limit
    // Note: Do NOT reset delegateTarget_ or delegateRetryCount_ - preserve for retry logic
}

void BusResetCoordinator::ResetDelegationRetryCounter() {
    if (delegateRetryCount_ > 0 || delegateSuppressed_) {
        ASFW_LOG(BusReset,
                 "Resetting delegation retry counter (was %u, suppressed=%d) - topology change or "
                 "gap=0 bypass",
                 delegateRetryCount_, delegateSuppressed_);
    }
    delegateRetryCount_ = 0;
    delegateSuppressed_ = false;
    // Note: Keep delegateTarget_ to detect target changes
}

} // namespace ASFW::Driver

