// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2024 ASFireWire Project
//
// BusManagerElectionDriver.cpp — see BusManagerElectionDriver.hpp

#include "BusManagerElectionDriver.hpp"
#include "../../Logging/Logging.hpp"
#include "../../Controller/ControllerTypes.hpp"

namespace ASFW::Bus {

BusManagerElectionDriver::BusManagerElectionDriver(Deps deps, ASFW::FW::RoleMode roleMode) noexcept
    : deps_(deps), roleMode_(roleMode) {}

void BusManagerElectionDriver::OnTopologyReady(const ASFW::Driver::TopologySnapshot& snap) noexcept {
    if (!active_) {
        return;
    }

    if (roleMode_ != ASFW::FW::RoleMode::FullBusManager) {
        return;
    }

    if (!snap.localNodeId.has_value() || !snap.irmNodeId.has_value()) {
        ASFW_LOG(Controller, "[BM Election] Skipping election: missing local ID or IRM ID");
        return;
    }

    const uint8_t localNodeId = snap.localNodeId.value();
    const uint8_t irmNodeId = snap.irmNodeId.value();
    const uint32_t generation = snap.generation;

    // Check if we are already contending for this or a newer generation
    if (inFlight_ && inFlightGen_ >= generation) {
        return;
    }

    bool abdicateObserved = false;
    if (deps_.csrResponder) {
        abdicateObserved = deps_.csrResponder->ConsumeAbdicate();
    }

    BmElectionInputs inputs{
        .mode = roleMode_,
        .generation = generation,
        .localId = localNodeId,
        .irmId = irmNodeId,
        .wasIncumbent = wasIncumbent_,
        .abdicateObserved = abdicateObserved,
    };

    DecisionAction action = fsm_.Decide(inputs);
    if (action == DecisionAction::DoNotContend) {
        return;
    }

    inFlight_ = true;
    inFlightGen_ = generation;

    if (action == DecisionAction::ContendImmediately) {
        ASFW_LOG(Controller, "[BM Election] Contending immediately for gen=%u as incumbent", generation);
        Contend(generation, localNodeId, irmNodeId, snap.busBase16);
    } else if (action == DecisionAction::ContendAfterGrace) {
        ASFW_LOG(Controller, "[BM Election] Contending after 125ms grace for gen=%u as challenger", generation);
        // Wait 125 ms challenger grace period
        const uint64_t delayNs = 125'000'000ULL;
        if (deps_.scheduler) {
            deps_.scheduler->DispatchAsyncAfter(delayNs, [this, generation, localNodeId, irmNodeId, busBase16 = snap.busBase16]() {
                if (!active_) {
                    return;
                }
                // Verify generation is still current before sending compare-swap
                if (deps_.asyncController) {
                    const auto state = deps_.asyncController->GetBusStateSnapshot();
                    if (state.generation16 != generation) {
                        ASFW_LOG(Controller, "[BM Election] Aborting grace period contention: generation changed from %u to %u",
                                 generation, state.generation16);
                        return;
                    }
                }
                Contend(generation, localNodeId, irmNodeId, busBase16);
            });
        } else {
            // Fallback for testing / stubs
            Contend(generation, localNodeId, irmNodeId, snap.busBase16);
        }
    }
}

void BusManagerElectionDriver::OnBusReset() noexcept {
    if (fsm_.Owner() == BmOwner::Local) {
        wasIncumbent_ = true;
    } else {
        wasIncumbent_ = false;
    }
    fsm_.Reset();
    inFlight_ = false;
    inFlightGen_ = 0;
}

void BusManagerElectionDriver::Stop() noexcept {
    active_ = false;
}

void BusManagerElectionDriver::Contend(uint32_t generation, uint8_t localNodeId, uint8_t irmNodeId, uint16_t busBase16) noexcept {
    if (!active_ || !deps_.asyncController) {
        inFlight_ = false;
        return;
    }

    // Verify generation is still current
    const auto state = deps_.asyncController->GetBusStateSnapshot();
    if (state.generation16 != generation) {
        ASFW_LOG(Controller, "[BM Election] Contention aborted: generation is stale (expected %u, current %u)",
                 generation, state.generation16);
        inFlight_ = false;
        return;
    }

    ASFW::Async::CompareSwapParams params{
        .destinationID = ASFW::Driver::ComposeNodeID(busBase16, irmNodeId),
        .addressHigh = 0,
        .addressLow = ASFW::FW::kCSR_BusManagerID,
        .compareValue = 0x3F,
        .swapValue = localNodeId,
        .speedCode = 0xFF // auto speed
    };

    ASFW_LOG(Controller, "[BM Election] Issuing CompareSwap to IRM node=0x%04X (phys=%u) for gen=%u",
             params.destinationID, irmNodeId, generation);

    deps_.asyncController->CompareSwap(params, [this, generation, localNodeId](ASFW::Async::AsyncStatus status, uint32_t oldValue, bool compareMatched) {
        HandleCompareSwapResult(generation, localNodeId, status, oldValue, compareMatched);
    });
}

void BusManagerElectionDriver::HandleCompareSwapResult(uint32_t generation, uint8_t localNodeId, ASFW::Async::AsyncStatus status, uint32_t oldValue, bool compareMatched) noexcept {
    if (!active_) {
        return;
    }

    inFlight_ = false;

    // Verify generation is still current
    if (deps_.asyncController) {
        const auto state = deps_.asyncController->GetBusStateSnapshot();
        if (state.generation16 != generation) {
            ASFW_LOG(Controller, "[BM Election] CompareSwap callback ignored: generation is stale (callback gen=%u, current gen=%u)",
                     generation, state.generation16);
            fsm_.IncrementStaleAbortCount();
            return;
        }
    }

    if (status != ASFW::Async::AsyncStatus::kSuccess) {
        ASFW_LOG(Controller, "[BM Election] CompareSwap failed with status %d (%s)",
                 static_cast<int>(status), ASFW::Async::ToString(status));
        return;
    }

    ElectionOutcome outcome = fsm_.InterpretOldValue(oldValue, localNodeId);
    switch (outcome) {
    case ElectionOutcome::WonBM:
        ASFW_LOG(Controller, "[BM Election] WON Bus Manager election! (oldValue=0x%X, compareMatched=%d)", oldValue, compareMatched);
        break;
    case ElectionOutcome::IncumbentReestablished:
        ASFW_LOG(Controller, "[BM Election] Re-established BM incumbency.");
        break;
    case ElectionOutcome::RemoteBM:
        ASFW_LOG(Controller, "[BM Election] Remote node 0x%02X won Bus Manager election (oldValue=0x%X).",
                 fsm_.OwnerId().value_or(0xFF), oldValue);
        break;
    default:
        break;
    }
}

} // namespace ASFW::Bus
