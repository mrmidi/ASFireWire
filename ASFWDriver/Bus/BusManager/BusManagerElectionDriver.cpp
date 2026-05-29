// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2024 ASFireWire Project
//
// BusManagerElectionDriver.cpp — see BusManagerElectionDriver.hpp

#include "BusManagerElectionDriver.hpp"
#include "../../Logging/Logging.hpp"
#include "../../Controller/ControllerTypes.hpp"
#include "../../Hardware/HardwareInterface.hpp"

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
            std::weak_ptr<BusManagerElectionDriver> weakSelf = shared_from_this();
            deps_.scheduler->DispatchAsyncAfter(delayNs, [weakSelf, generation, localNodeId, irmNodeId, busBase16 = snap.busBase16]() {
                auto self = weakSelf.lock();
                if (!self) {
                    return;
                }
                if (!self->active_) {
                    return;
                }
                // Verify generation is still current before sending compare-swap
                if (self->deps_.asyncController) {
                    const auto state = self->deps_.asyncController->GetBusStateSnapshot();
                    if (state.generation16 != generation) {
                        ASFW_LOG(Controller, "[BM Election] Aborting grace period contention: generation changed from %u to %u",
                                 generation, state.generation16);
                        self->inFlight_ = false;
                        return;
                    }
                }
                self->Contend(generation, localNodeId, irmNodeId, busBase16);
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
    if (inFlightHandle_) {
        if (deps_.asyncController) {
            deps_.asyncController->Cancel(inFlightHandle_);
        }
        inFlightHandle_ = {};
    }
}

void BusManagerElectionDriver::Stop() noexcept {
    active_ = false;
    if (inFlightHandle_) {
        if (deps_.asyncController) {
            deps_.asyncController->Cancel(inFlightHandle_);
        }
        inFlightHandle_ = {};
    }
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

    // Local Loopback Compare-Swap Branching (FW-14)
    if (irmNodeId == localNodeId) {
        ASFW_LOG(Controller, "[BM Election] Local node is IRM; routing CompareSwap through local CSRControl loopback");
        if (deps_.hardware == nullptr) {
            ASFW_LOG(Controller, "[BM Election] Cannot perform local CompareSwap: hardware interface is null");
            inFlight_ = false;
            if (observer_) {
                observer_->OnBMElectionFailed(generation, ASFW::Async::AsyncStatus::kHardwareError);
            }
            return;
        }

        // SelectCode for BUS_MANAGER_ID is 0.
        const auto result = deps_.hardware->CompareSwapLocalIRMResource(0, 0x3F, localNodeId);

        if (result.status != ASFW::Driver::LocalCSRLockResult::Status::Success) {
            ASFW_LOG(Controller, "[BM Election] Local CompareSwap failed (status=%d)",
                     static_cast<int>(result.status));
            inFlight_ = false;
            if (observer_) {
                observer_->OnBMElectionFailed(generation, ASFW::Async::AsyncStatus::kHardwareError);
            }
            return;
        }

        // Invoke HandleCompareSwapResult synchronously since it's a local hardware lock sequence
        HandleCompareSwapResult(generation, localNodeId, ASFW::Async::AsyncStatus::kSuccess, result.oldValue, result.compareMatched);
        return;
    }

    ASFW::Async::CompareSwapParams params{
        .destinationID = ASFW::Driver::ComposeNodeID(busBase16, irmNodeId),
        .addressHigh = ASFW::FW::kCSRRegSpaceHi,
        .addressLow = ASFW::FW::kCSR_BusManagerID,
        .compareValue = 0x3F,
        .swapValue = localNodeId,
        .speedCode = static_cast<uint8_t>(ASFW::FW::Speed::S100)
    };

    ASFW_LOG(Controller, "[BM Election] Issuing CompareSwap to IRM node=0x%04X (phys=%u) for gen=%u",
             params.destinationID, irmNodeId, generation);

    std::weak_ptr<BusManagerElectionDriver> weakSelf = shared_from_this();
    inFlightHandle_ = deps_.asyncController->CompareSwap(params, [weakSelf, generation, localNodeId](ASFW::Async::AsyncStatus status, uint32_t oldValue, bool compareMatched) {
        auto self = weakSelf.lock();
        if (!self) {
            return;
        }
        self->inFlightHandle_ = {}; // clear in-flight handle
        self->HandleCompareSwapResult(generation, localNodeId, status, oldValue, compareMatched);
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
        if (observer_) {
            observer_->OnBMElectionFailed(generation, status);
        }
        return;
    }

    ElectionOutcome outcome = fsm_.InterpretOldValue(oldValue, localNodeId);
    switch (outcome) {
    case ElectionOutcome::WonBM:
        ASFW_LOG(Controller, "[BM Election] WON Bus Manager election! (oldValue=0x%X, compareMatched=%d)", oldValue, compareMatched);
        if (observer_) {
            observer_->OnLocalWonBM(generation, localNodeId);
        }
        break;
    case ElectionOutcome::IncumbentReestablished:
        ASFW_LOG(Controller, "[BM Election] Re-established BM incumbency.");
        if (observer_) {
            observer_->OnLocalWonBM(generation, localNodeId);
        }
        break;
    case ElectionOutcome::RemoteBM:
        ASFW_LOG(Controller, "[BM Election] Remote node 0x%02X won Bus Manager election (oldValue=0x%X).",
                 fsm_.OwnerId().value_or(0xFF), oldValue);
        if (observer_) {
            observer_->OnRemoteBM(generation, fsm_.OwnerId().value_or(0xFF));
        }
        break;
    default:
        if (observer_) {
            observer_->OnBMElectionFailed(generation, ASFW::Async::AsyncStatus::kLockCompareFail);
        }
        break;
    }
}

} // namespace ASFW::Bus
