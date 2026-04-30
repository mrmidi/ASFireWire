#include "AsyncSubsystem.hpp"

#include "Tx/Submitter.hpp"

#include "../Logging/Logging.hpp"
#include "Track/LabelAllocator.hpp"
#include "Track/PayloadRegistry.hpp"

#include <DriverKit/IOLib.h>

namespace ASFW::Async {

void AsyncSubsystem::OnBusResetBegin(uint8_t nextGen) {
    // CRITICAL: Follow Linux core-transaction.c:fw_core_handle_bus_reset() ordering
    // 1. Gate new submissions FIRST
    // 2. Cancel OLD generation transactions SECOND
    // 3. Let HARDWARE set generation via synthetic bus reset packet
    // This ordering prevents race: generation comes from hardware, not manual increment

    // Step 1: Gate new submissions
    // Any new RegisterTx() calls will be blocked until bus reset completes
    is_bus_reset_in_progress_.store(1, std::memory_order_release);

    // NOTE: Generation will be updated by hardware via synthetic bus reset packet
    // in RxPath, NOT manually here. This prevents race between OnBusResetBegin
    // and the AR synthetic packet handler.

    // Step 2: Cancel transactions from OLD generation only
    // Read current generation from tracker (set by previous bus reset)
    const uint16_t oldGen = generationTracker_ ? generationTracker_->GetCurrentState().generation16 : 0;

    if (tracking_) {
        // Cancel any lingering transactions (all generations) to guarantee label bitmap is clean.
        tracking_->CancelAllAndFreeLabels();
        // Cancel transactions belonging to oldGen (precise, not ~0u!)
        tracking_->CancelByGeneration(oldGen);

        // Hard-clear bitmap to evict any leaked bits that lack corresponding transactions
        if (auto* alloc = tracking_->GetLabelAllocator()) {
            alloc->ClearBitmap();
        }
    }

    // Step 3: Bump payload epoch for deferred cleanup (to nextGen)
    if (tracking_ && tracking_->Payloads()) {
        tracking_->Payloads()->SetEpoch(nextGen);
    }

    ASFW_LOG(Async, "OnBusResetBegin: cancelled oldGen=%u transactions, payload epoch→%u (hw will set gen)",
             oldGen, nextGen);
}

void AsyncSubsystem::OnBusResetComplete(uint8_t stableGen) {
    is_bus_reset_in_progress_.store(0, std::memory_order_release);
    ASFW_LOG(Async, "OnBusResetComplete: gen=%u", stableGen);
}

void AsyncSubsystem::RearmATContexts() {
    // OHCI §7.2.3.2 Step 7: Re-arm AT contexts after busReset cleared
    // CRITICAL: This is called by ControllerCore AFTER:
    //   1. AT contexts stopped (active=0)
    //   2. IntEvent.busReset cleared
    //   3. Self-ID complete
    //   4. Config ROM restored
    //   5. AsynchronousRequestFilter re-enabled
    //
    // Calling this earlier (e.g., in OnBusReset) prevents busReset clearing because
    // ControllerCore checks AT contexts are inactive before clearing the interrupt.

    ASFW_LOG(Async, "Re-arming AT contexts for new generation (OHCI §7.2.3.2 step 7)");

    // Step 6 from §7.2.3.2: Read NodeID (should be valid - Self-ID already completed)
    // CRITICAL: No polling here! This is still called from interrupt context.
    // Since we only call RearmATContexts() AFTER Self-ID complete, NodeID should be valid.
    if (hardware_) {
        constexpr uint32_t kNodeIDValidBit = 0x80000000u;
        constexpr uint16_t kUnassignedBus = 0x03FFu;

        uint32_t nodeIdReg = 0;

        // Poll briefly for NodeID valid. Keep the wait bounded (~10 ms) since we're
        // still on the interrupt workloop.
        for (uint32_t attempt = 0; attempt < 100; ++attempt) {
            nodeIdReg = hardware_->ReadNodeID();
            if ((nodeIdReg & kNodeIDValidBit) != 0) {
                break;
            }
            IODelay(100); // 100 µs
        }

        const bool idValid = (nodeIdReg & kNodeIDValidBit) != 0;
        if (!idValid) {
            if (generationTracker_) {
                generationTracker_->OnSelfIDComplete(0);
            }
            ASFW_LOG(Async,
                     "WARNING: NodeID never reported valid state (reg=0x%08x). "
                     "Async transmit remains gated.",
                     nodeIdReg);
        } else {
            const uint16_t rawBus = static_cast<uint16_t>((nodeIdReg >> 6) & 0x03FFu);
            const uint8_t nodeNumber = static_cast<uint8_t>(nodeIdReg & 0x3F);
            // Per IEEE 1394-1995 §8.3.2.3.2: source_ID uses broadcast bus (0x3ff) if unassigned
            // NEVER substitute bus=0, as it's semantically different from unassigned broadcast bus
            const uint16_t nodeID = static_cast<uint16_t>((rawBus << 6) | nodeNumber);

            if (generationTracker_) {
                generationTracker_->OnSelfIDComplete(nodeID);
            }

            if (rawBus == kUnassignedBus) { // NOSONAR(cpp:S3923): branches log different diagnostic messages
                ASFW_LOG(Async,
                         "NodeID valid: using broadcast bus (0x3ff) for source field (raw=0x%08x node=%u)",
                         nodeIdReg,
                         nodeNumber);
            } else {
                ASFW_LOG(Async,
                         "NodeID locked: bus=%u node=%u (raw=0x%08x)",
                         rawBus,
                         nodeNumber,
                         nodeIdReg);
            }
        }
    }

    // Re-arm AT contexts: ContextManager is the authoritative owner.
    if (!contextManager_) { // NOSONAR(cpp:S3923): branches log different diagnostic messages
        ASFW_LOG(Async, "RearmATContexts: ContextManager unavailable - cannot rearm");
        return;
    }

    // ContextManager owns contexts and manages the DMA; AT contexts remain
    // idle until the first SubmitChain() per Apple's implementation.
    ASFW_LOG(Async, "RearmATContexts: handled by ContextManager (AT contexts remain idle)");
    return;
}

bool AsyncSubsystem::EnsureATContextsRunning(const char* reason) {
    // Per Apple's implementation: AT contexts are NOT pre-armed.
    // They arm themselves during SubmitChain() when transitioning from idle→active.
    // This function is retained for API compatibility but no longer attempts re-arming.
    (void)reason;  // Unused
    return false;
}

void AsyncSubsystem::StopATContextsOnly() {
    // Bus Reset Recovery per OHCI §7.2.3.2, §C.2
    // CRITICAL: Only stop AT contexts - AR contexts continue processing
    // Called by BusResetCoordinator during QuiescingAT state
    if (contextManager_) {
        const kern_return_t stopKr = contextManager_->stopAT();
        if (stopKr != kIOReturnSuccess) {
            ASFW_LOG(Async, "StopATContextsOnly: ContextManager::stopAT failed (kr=0x%08x)", stopKr);
        }
    } else {
        ASFW_LOG(Async, "StopATContextsOnly: ContextManager not present - nothing to stop");
    }
    // Notify Submitter that AT contexts have been stopped so it can reset internal state
    if (submitter_) {
        submitter_->OnATContextsStopped();
    }
    // DO NOT stop AR contexts - they continue per §C.3
}

void AsyncSubsystem::FlushATContexts() {
    if (!txnMgr_) {
        return;
    }
    (void)DrainTxCompletions(nullptr);
}

void AsyncSubsystem::ConfirmBusGeneration(uint8_t confirmedGeneration) {
    // Coordinate bus reset based on synthetic packet from controller
    // CRITICAL: This is called when AR Request receives the Bus-Reset packet
    // BEFORE the main interrupt handler sees IntEvent.busReset
    //
    // Linux equivalent: handle_ar_packet() evt_bus_reset → fw_core_handle_bus_reset()
    // Updates generation, gates AT contexts, keeps AR running

    // ConfirmBusGeneration: called when a new generation is confirmed (e.g. after
    // Self-ID decoding). This is the AUTHORITATIVE generation from SelfIDCount register.
    // This is the ONLY place where generation should be set.
    ASFW_LOG(Async, "ConfirmBusGeneration: Confirmed generation %u (from SelfIDCount register)", confirmedGeneration);

    const auto currentState = generationTracker_ ? generationTracker_->GetCurrentState() : Bus::GenerationTracker::BusState{};

    // Set the generation from hardware (SelfIDCount register is authoritative per OHCI §11.2)
    if (generationTracker_) {
        generationTracker_->OnSyntheticBusReset(confirmedGeneration);
        ASFW_LOG(Async, "GenerationTracker updated: %u→%u", currentState.generation8, confirmedGeneration);
    }

    // No redundant cancel here - already done in OnBusResetBegin
    if (tracking_) {
        ASFW_LOG(Async, "Generation confirmed via Tracking actor (no redundant cancel)");
    }

    // Annexe C behavior: cancel request payloads belonging to the old generation
    // but keep AR contexts running. Use PayloadRegistry to cancel payloads
    // from the previous 8-bit generation. We treat generation numbers as 8-bit
    // and cancel payloads with epoch <= oldGen.
    if (contextManager_) {
        auto* pr = contextManager_->Payloads();
        if (pr) {
            // Compute previous generation (wrap-around aware)
            const uint32_t oldGen = (confirmedGeneration == 0) ? 0xFFu : (static_cast<uint32_t>(confirmedGeneration) - 1u);
            pr->CancelByEpoch(oldGen, PayloadRegistry::CancelMode::Deferred);
            // Advance registry epoch to the confirmed generation so new submissions
            // are tagged with the new epoch.
            pr->SetEpoch(static_cast<uint32_t>(confirmedGeneration));
            ASFW_LOG(Async, "PayloadRegistry: canceled epoch <= %u and set epoch=%u", oldGen, confirmedGeneration);
        }
    }

    ASFW_LOG(Async, "ConfirmBusGeneration complete - async subsystem coordinated for new generation");
}

} // namespace ASFW::Async
