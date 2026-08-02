// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024 ASFireWire Project
//
// PowerLinkPolicyCoordinator.cpp — see PowerLinkPolicyCoordinator.hpp

#include "PowerLinkPolicyCoordinator.hpp"

namespace ASFW::Bus {

using namespace ASFW::FW;

PowerLinkPolicyCoordinator::PowerLinkPolicyCoordinator(PowerLinkPolicyConfig config) noexcept
    : config_(config) {}

void PowerLinkPolicyCoordinator::OnBusResetStarted(uint32_t generation) noexcept {
    // Preserve cumulative counters
    const uint32_t total = snapshot_.totalAttempts;
    const uint32_t suppressed = snapshot_.suppressedCount;
    const uint32_t submitted = snapshot_.linkOnSubmittedCount;
    const uint32_t success = snapshot_.linkOnSuccessCount;
    const uint32_t failure = snapshot_.linkOnFailureCount;
    const uint32_t stale = snapshot_.staleGenerationDrops;

    snapshot_ = {};
    snapshot_.generation = generation;
    snapshot_.totalAttempts = total;
    snapshot_.suppressedCount = suppressed;
    snapshot_.linkOnSubmittedCount = submitted;
    snapshot_.linkOnSuccessCount = success;
    snapshot_.linkOnFailureCount = failure;
    snapshot_.staleGenerationDrops = stale;
}

void PowerLinkPolicyCoordinator::Evaluate(const PowerLinkPolicyInputs& inputs,
                                          ILinkOnExecutor& executor) noexcept {
    auto effectiveInputs = inputs;
    const auto estimate = EstimatePowerBudget(effectiveInputs);
    effectiveInputs.powerBudgetStatus = estimate.status;

    snapshot_.generation = inputs.generation;
    snapshot_.powerBudgetStatus = effectiveInputs.powerBudgetStatus;

    snapshot_.powerAvailableMilliWatts = estimate.availableMilliWatts;
    snapshot_.powerRequiredMilliWatts = estimate.requiredMilliWatts;
    snapshot_.unknownPowerClassNodes = estimate.unknownPowerClassNodes;
    
    const auto candidates = BuildCandidates(effectiveInputs);
    snapshot_.eligibleNodeCount = static_cast<uint32_t>(candidates.size());

    snapshot_.lastDecision = Plan(effectiveInputs, candidates);
    snapshot_.lastAction = PowerPolicyAction::None;
    snapshot_.targetNodeCount = 0;
    
    for (const auto& c : candidates) {
        if (snapshot_.targetNodeCount >= snapshot_.targetNodes.size()) {
            break;
        }
        snapshot_.targetNodes[snapshot_.targetNodeCount++] = c.nodeId;
    }

    if (snapshot_.lastDecision != PowerPolicyDecision::LinkOnRequired) {
        if (snapshot_.lastDecision != PowerPolicyDecision::None &&
            snapshot_.lastDecision < PowerPolicyDecision::NoEligibleNodes) {
            snapshot_.suppressedCount++;
        }
        return;
    }

    snapshot_.lastAction = PowerPolicyAction::SendLinkOnPackets;

    for (const auto& c : candidates) {
        const bool ok = executor.SendLinkOnPacket(effectiveInputs.generation,
                                                  effectiveInputs.busBase16,
                                                  c.nodeId);
        snapshot_.linkOnSubmittedCount++;
        if (ok) {
            snapshot_.linkOnSuccessCount++;
        } else {
            snapshot_.linkOnFailureCount++;
        }
    }

    snapshot_.attemptsThisGeneration++;
    snapshot_.totalAttempts++;
}

PowerPolicyDecision PowerLinkPolicyCoordinator::Plan(const PowerLinkPolicyInputs& inputs,
                                                      const std::vector<PowerLinkNodeEvidence>& candidates) const noexcept {
    if (!inputs.topologyValid || inputs.topology == nullptr) {
        return PowerPolicyDecision::SuppressedByTopology;
    }

    if (inputs.roleMode == RoleMode::ClientOnly) {
        return PowerPolicyDecision::SuppressedByRoleMode;
    }

    if (inputs.powerPolicyLevel < Driver::PowerPolicyLevel::LinkOnAllowed) {
        return PowerPolicyDecision::SuppressedByPolicyLevel;
    }

    if (!IsAllowedActor(inputs)) {
        return PowerPolicyDecision::SuppressedNotBMOrFallbackIRM;
    }

    if (auto decision = BudgetDecision(inputs.powerBudgetStatus);
        decision != PowerPolicyDecision::None) {
        return decision;
    }

    if (candidates.empty()) {
        return PowerPolicyDecision::NoEligibleNodes;
    }

    if (snapshot_.attemptsThisGeneration >= config_.maxLinkOnAttemptsPerGeneration) {
        return PowerPolicyDecision::LinkOnAlreadyAttemptedThisGeneration;
    }

    return PowerPolicyDecision::LinkOnRequired;
}

PowerPolicyDecision
PowerLinkPolicyCoordinator::BudgetDecision(PowerBudgetStatus status) const noexcept {
    if (config_.requireKnownPowerBudget &&
        status == PowerBudgetStatus::Unknown &&
        !config_.allowWhenPowerBudgetUnknown) {
        return PowerPolicyDecision::DeferredPowerBudgetUnknown;
    }

    if (status == PowerBudgetStatus::Insufficient) {
        return PowerPolicyDecision::DeferredInsufficientPower;
    }

    return PowerPolicyDecision::None;
}

PowerBudgetEstimate
PowerLinkPolicyCoordinator::EstimatePowerBudget(const PowerLinkPolicyInputs& inputs) const noexcept {
    PowerBudgetEstimate estimate{};

    if (!inputs.topologyValid || inputs.topology == nullptr) {
        return estimate;
    }

    // IEEE 1394 Self-ID pwr is a capability class, not a measured wattage.
    // Map it conservatively: reserved/unknown classes block automatic Link-On.
    // cross-validated with Linux: phy-packet-definitions.h:174-182
    for (const auto& node : inputs.topology->physical.nodes) {
        switch (static_cast<ASFW::Driver::PowerClass>(node.powerClass & 0x07u)) {
        case ASFW::Driver::PowerClass::NoPower:
            break;
        case ASFW::Driver::PowerClass::SelfPower_15W:
            estimate.availableMilliWatts += 15000;
            break;
        case ASFW::Driver::PowerClass::SelfPower_30W:
            estimate.availableMilliWatts += 30000;
            break;
        case ASFW::Driver::PowerClass::SelfPower_45W:
            estimate.availableMilliWatts += 45000;
            break;
        case ASFW::Driver::PowerClass::BusPowered_UpTo3W:
            estimate.requiredMilliWatts += 3000;
            break;
        case ASFW::Driver::PowerClass::Reserved101:
            estimate.unknownPowerClassNodes++;
            break;
        case ASFW::Driver::PowerClass::BusPowered_3W_plus3:
            estimate.availableMilliWatts += 3000;
            estimate.requiredMilliWatts += 3000;
            break;
        case ASFW::Driver::PowerClass::BusPowered_3W_plus7:
            estimate.availableMilliWatts += 7000;
            estimate.requiredMilliWatts += 3000;
            break;
        }
    }

    if (estimate.unknownPowerClassNodes != 0) {
        estimate.status = PowerBudgetStatus::Unknown;
    } else if (estimate.requiredMilliWatts <= estimate.availableMilliWatts) {
        estimate.status = PowerBudgetStatus::Sufficient;
    } else {
        estimate.status = PowerBudgetStatus::Insufficient;
    }

    return estimate;
}

std::vector<PowerLinkNodeEvidence>
PowerLinkPolicyCoordinator::BuildCandidates(const PowerLinkPolicyInputs& inputs) const {
    std::vector<PowerLinkNodeEvidence> out;

    if (inputs.topology == nullptr) {
        return out;
    }

    // cross-validated with Linux: core-topology.c:26-36.
    // Linux reads the link_active (L-bit) from Self-ID packets.
    for (const auto& node : inputs.topology->physical.nodes) {
        PowerLinkNodeEvidence ev{};
        ev.nodeId = node.physicalId;
        ev.isLocal = node.physicalId == inputs.localNodeId;
        ev.isRoot = node.physicalId == inputs.rootNodeId;
        ev.linkActive = node.linkActive;
        ev.phyPresent = true;
        ev.powerClass = node.powerClass;
        ev.powerClassKnown = true;

        if (config_.skipLocalNode && ev.isLocal) {
            continue;
        }

        if (config_.skipRootNode && ev.isRoot) {
            continue;
        }

        if (!ev.phyPresent) {
            continue;
        }

        if (ev.linkActive) {
            continue;
        }

        if (!config_.allowPhyOnlyTargets && node.maxSpeedMbps == 0) {
            // Assume 0 maxSpeedMbps means PHY-only repeater for now
            continue;
        }

        ev.eligibleForLinkOn = true;
        ev.reason = LinkOnTargetReason::LinkInactiveSelfID;
        out.push_back(ev);

        if (out.size() >= config_.maxTargetsPerEvaluation) {
            break;
        }
    }

    return out;
}

/**
 * @brief Checks if the local node is an allowed actor for BM/fallback duties.
 * cross-validated with Linux: core-card.c:347-352.
 */
bool PowerLinkPolicyCoordinator::IsAllowedActor(const PowerLinkPolicyInputs& inputs) const noexcept {
    const bool activeBM =
        inputs.roleMode == RoleMode::FullBusManager &&
        inputs.localIsBM;

    const bool fallbackIRM =
        (inputs.roleMode == RoleMode::IRMResourceHost ||
         inputs.roleMode == RoleMode::FullBusManager) &&
        inputs.localIsIRM &&
        inputs.irmFallbackGateOpen &&
        inputs.irmFallbackNoBMDetected;

    return activeBM || fallbackIRM;
}

} // namespace ASFW::Bus
