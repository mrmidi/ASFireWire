// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2024 ASFireWire Project
//
// BusManagerElection.cpp — see BusManagerElection.hpp

#include "BusManagerElection.hpp"

namespace ASFW::Bus {

DecisionAction BusManagerElection::Decide(const BmElectionInputs& inputs) noexcept {
    if (inputs.mode != ASFW::FW::RoleMode::FullBusManager) {
        return DecisionAction::DoNotContend;
    }
    if (!inputs.irmId.has_value()) {
        return DecisionAction::DoNotContend;
    }

    lastElectionGen_ = inputs.generation;

    if (inputs.wasIncumbent && !inputs.abdicateObserved) {
        return DecisionAction::ContendImmediately;
    }

    return DecisionAction::ContendAfterGrace;
}

ElectionOutcome BusManagerElection::InterpretOldValue(uint32_t oldValue, uint8_t localId) noexcept {
    lastOldValue_ = oldValue;

    if (oldValue == 0x3F) {
        owner_ = BmOwner::Local;
        ownerId_ = localId;
        return ElectionOutcome::WonBM;
    }

    if (oldValue == localId) {
        owner_ = BmOwner::Local;
        ownerId_ = localId;
        return ElectionOutcome::IncumbentReestablished;
    }

    owner_ = BmOwner::Remote;
    ownerId_ = static_cast<uint8_t>(oldValue & 0x3Fu);
    return ElectionOutcome::RemoteBM;
}

void BusManagerElection::Reset() noexcept {
    owner_ = BmOwner::None;
    ownerId_ = std::nullopt;
    lastOldValue_ = 0x3F;
}

} // namespace ASFW::Bus
