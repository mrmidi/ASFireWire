// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2024 ASFireWire Project
//
// GapPolicyCoordinator.cpp — see GapPolicyCoordinator.hpp

#include "GapPolicyCoordinator.hpp"
#include <algorithm>

namespace ASFW::Bus {

using namespace ASFW::FW;

static constexpr std::array<uint8_t, 16> kGapCount1394aByMaxHops{
    63, 5, 7, 8, 10, 13, 16, 18, 21, 24, 26, 29, 32, 35, 37, 40
};

GapPolicyCoordinator::GapPolicyCoordinator(GapPolicyConfig config) noexcept
    : config_(config) {}

void GapPolicyCoordinator::OnBusResetStarted(uint32_t generation) noexcept {
    // Preserve cumulative counters
    const uint32_t total = snapshot_.totalAttempts;
    const uint32_t suppressed = snapshot_.suppressedCount;

    snapshot_ = {};
    snapshot_.generation = generation;
    snapshot_.totalAttempts = total;
    snapshot_.suppressedCount = suppressed;
}

void GapPolicyCoordinator::Evaluate(const GapPolicyInputs& inputs,
                                    IGapPolicyExecutor& executor) noexcept {
    snapshot_.generation = inputs.generation;
    snapshot_.currentGapCount = inputs.currentGapCount;
    snapshot_.gapCountConsistent = inputs.gapCountConsistent;
    snapshot_.maxHopsKnown = inputs.maxHopsKnown;
    snapshot_.maxHopsFromRoot = inputs.maxHopsFromRoot;
    snapshot_.betaRepeatersKnown = inputs.betaRepeatersKnown;
    snapshot_.betaRepeatersPresent = inputs.betaRepeatersPresent;

    const uint32_t key = StableTopologyKey(inputs);
    if (key != stableTopologyKey_) {
        stableTopologyKey_ = key;
        attemptsThisStableTopology_ = 0;
        mismatchRepairsThisStableTopology_ = 0;
    }

    GapComputationSource source{GapComputationSource::None};
    const uint8_t expectedGap = ComputeExpectedGapCount(inputs, &source);

    snapshot_.expectedGapCount = expectedGap;
    snapshot_.requestedGapCount = expectedGap;
    snapshot_.computationSource = source;
    snapshot_.lastDecision = Plan(inputs);
    snapshot_.lastAction = GapPolicyAction::None;
    snapshot_.resetRequested = false;
    snapshot_.combinedWithRootSelection = false;
    snapshot_.targetRoot = inputs.rootNodeId;

    switch (snapshot_.lastDecision) {
    case GapPolicyDecision::GapMismatchRequiresLongReset:
    case GapPolicyDecision::GapOptimizationRequired: {
        const bool longReset =
            snapshot_.lastDecision == GapPolicyDecision::GapMismatchRequiresLongReset ||
            !config_.useShortResetForPureOptimization;

        const uint8_t targetRoot =
            inputs.rootSelectionRequired && inputs.selectedRootForRootPolicy != 0x3F
                ? inputs.selectedRootForRootPolicy
                : inputs.rootNodeId;

        snapshot_.targetRoot = targetRoot;
        snapshot_.combinedWithRootSelection = inputs.rootSelectionRequired;
        
        if (inputs.rootSelectionRequired) {
            snapshot_.lastAction = longReset ? GapPolicyAction::ForceRootWithGapAndLongReset
                                             : GapPolicyAction::ForceRootWithGapAndShortReset;
        } else {
            snapshot_.lastAction = longReset ? GapPolicyAction::GapOnlyLongReset
                                             : GapPolicyAction::GapOnlyShortReset;
        }

        const bool ok = executor.ForceRootAndGapResetForBMPolicy(inputs.generation,
                                                                 targetRoot,
                                                                 longReset,
                                                                 expectedGap);
        if (!ok) {
            snapshot_.lastDecision = GapPolicyDecision::FailedExecutorUnavailable;
            return;
        }

        snapshot_.resetRequested = true;
        snapshot_.totalAttempts++;

        if (snapshot_.lastDecision == GapPolicyDecision::GapMismatchRequiresLongReset) {
            mismatchRepairsThisStableTopology_++;
            snapshot_.mismatchRepairsThisTopology = mismatchRepairsThisStableTopology_;
        } else {
            attemptsThisStableTopology_++;
            snapshot_.attemptsThisTopology = attemptsThisStableTopology_;
        }

        return;
    }

    case GapPolicyDecision::FailedRetryLimit:
        snapshot_.retryLimitHit = 1;
        return;

    default:
        if (snapshot_.lastDecision != GapPolicyDecision::None &&
            snapshot_.lastDecision < GapPolicyDecision::AlreadyOptimal) {
            snapshot_.suppressedCount++;
        }
        return;
    }
}

GapPolicyDecision GapPolicyCoordinator::Plan(const GapPolicyInputs& inputs) const noexcept {
    if (!inputs.topologyValid || inputs.topology == nullptr) {
        return GapPolicyDecision::SuppressedByTopology;
    }

    if (inputs.roleMode == RoleMode::ClientOnly) {
        return GapPolicyDecision::SuppressedByRoleMode;
    }

    if (inputs.activityLevel < FullBMActivityLevel::GapPolicyAllowed) {
        return GapPolicyDecision::SuppressedByActivityLevel;
    }

    if (!IsAllowedActor(inputs)) {
        return GapPolicyDecision::SuppressedNotBMOrFallbackIRM;
    }

    if (inputs.topology->nodeCount <= 1) {
        return GapPolicyDecision::SuppressedSingleNodeBus;
    }

    if (!inputs.maxHopsKnown) {
        return GapPolicyDecision::DeferMaxHopsUnavailable;
    }

    if (!inputs.betaRepeatersKnown && !config_.optimizeWhenBetaRepeatersUnknown) {
        return GapPolicyDecision::DeferBetaRepeaterUnknown;
    }

    GapComputationSource source{GapComputationSource::None};
    const uint8_t expectedGap = ComputeExpectedGapCount(inputs, &source);

    if (!inputs.gapCountConsistent) {
        if (mismatchRepairsThisStableTopology_ >= config_.maxMismatchRepairsPerStableTopology) {
            return GapPolicyDecision::FailedRetryLimit;
        }
        return GapPolicyDecision::GapMismatchRequiresLongReset;
    }

    if (inputs.currentGapCount == expectedGap) {
        return GapPolicyDecision::AlreadyOptimal;
    }

    if (attemptsThisStableTopology_ >= config_.maxOptimizationAttemptsPerStableTopology) {
        return GapPolicyDecision::FailedRetryLimit;
    }

    return GapPolicyDecision::GapOptimizationRequired;
}

uint8_t GapPolicyCoordinator::ComputeExpectedGapCount(const GapPolicyInputs& inputs,
                                                      GapComputationSource* source) const noexcept {
    if (source != nullptr) {
        *source = GapComputationSource::None;
    }

    if (!inputs.maxHopsKnown) {
        if (source != nullptr) {
            *source = GapComputationSource::ExistingGapPreserved;
        }
        return inputs.currentGapCount;
    }

    if (!inputs.betaRepeatersKnown && !config_.optimizeWhenBetaRepeatersUnknown) {
        if (source != nullptr) {
            *source = GapComputationSource::DefaultSafe63;
        }
        return 63;
    }

    if (inputs.betaRepeatersKnown && inputs.betaRepeatersPresent) {
        if (source != nullptr) {
            *source = GapComputationSource::DefaultSafe63;
        }
        return 63;
    }

    if (config_.enable1394aTable &&
        inputs.maxHopsFromRoot < kGapCount1394aByMaxHops.size()) {
        if (source != nullptr) {
            *source = GapComputationSource::Table1394a;
        }
        return kGapCount1394aByMaxHops[inputs.maxHopsFromRoot];
    }

    if (source != nullptr) {
        *source = GapComputationSource::DefaultSafe63;
    }
    return 63;
}

bool GapPolicyCoordinator::IsAllowedActor(const GapPolicyInputs& inputs) const noexcept {
    const bool activeBM =
        inputs.roleMode == ASFW::FW::RoleMode::FullBusManager &&
        inputs.localIsBM;

    const bool fallbackIRM =
        (inputs.roleMode == ASFW::FW::RoleMode::IRMResourceHost ||
         inputs.roleMode == ASFW::FW::RoleMode::FullBusManager) &&
        inputs.localIsIRM &&
        inputs.irmFallbackGateOpen &&
        inputs.irmFallbackNoBMDetected;

    return activeBM || fallbackIRM;
}

uint32_t GapPolicyCoordinator::StableTopologyKey(const GapPolicyInputs& inputs) const noexcept {
    if (inputs.topology == nullptr) {
        return 0;
    }

    const auto& topo = *inputs.topology;

    uint32_t h = 2166136261u;
    auto mix = [&h](uint32_t v) {
        h ^= v;
        h *= 16777619u;
    };

    mix(topo.nodeCount);
    mix(topo.localNodeId);
    mix(topo.irmNodeId);

    for (const auto& node : topo.physical.nodes) {
        mix(node.physicalId);
        mix(node.portCount);
        mix(node.linkActive ? 1u : 0u);
        for (uint8_t p = 0; p < node.portCount; ++p) {
            const auto& link = node.links[p];
            if (link.connected) {
                mix((static_cast<uint32_t>(node.physicalId) << 16) |
                    (static_cast<uint32_t>(p) << 8) |
                    static_cast<uint32_t>(link.remoteNodeId));
            }
        }
    }

    return h;
}

} // namespace ASFW::Bus
