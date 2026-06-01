// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2024 ASFireWire Project
//
// RootSelectionCoordinator.cpp — see RootSelectionCoordinator.hpp

#include "RootSelectionCoordinator.hpp"
#include <algorithm>

namespace ASFW::Bus {

using namespace ASFW::FW;

namespace {

const ASFW::Driver::TopologyNodeRecord*
FindNode(const ASFW::Driver::TopologySnapshot& topology, uint8_t physicalId) noexcept {
    for (const auto& node : topology.physical.nodes) {
        if (node.physicalId == physicalId) {
            return &node;
        }
    }
    return nullptr;
}

} // anonymous namespace

RootSelectionCoordinator::RootSelectionCoordinator(RootSelectionConfig config) noexcept
    : config_(config) {}

void RootSelectionCoordinator::OnBusResetStarted(uint32_t generation) noexcept {
    // Preserve cumulative counters
    const uint32_t total = snapshot_.totalAttempts;
    const uint32_t suppressed = snapshot_.suppressedCount;
    const uint32_t stale = snapshot_.staleGenerationDrops;

    snapshot_ = {};
    snapshot_.generation = generation;
    snapshot_.totalAttempts = total;
    snapshot_.suppressedCount = suppressed;
    snapshot_.staleGenerationDrops = stale;
}

void RootSelectionCoordinator::Evaluate(const RootSelectionInputs& inputs,
                                        IRootSelectionExecutor& executor) noexcept {
    snapshot_.generation = inputs.generation;
    snapshot_.previousRoot = inputs.rootNodeId;
    snapshot_.currentGapCount = inputs.currentGapCount;

    const uint32_t key = StableTopologyKey(inputs);
    if (key != stableTopologyKey_) {
        stableTopologyKey_ = key;
        attemptsThisStableTopology_ = 0;
    }

    snapshot_.lastDecision = Plan(inputs);
    snapshot_.lastAction = RootSelectionAction::None;
    snapshot_.selectedRoot = 0x3F;
    snapshot_.resetRequested = false;
    snapshot_.retryLimitHit = false;

    switch (snapshot_.lastDecision) {
    case RootSelectionDecision::SelectLocalRoot:
    case RootSelectionDecision::SelectRemoteRoot: {
        const auto candidate = SelectCandidate(inputs);
        if (!candidate.has_value()) {
            snapshot_.lastDecision = RootSelectionDecision::FailedNoCandidate;
            return;
        }

        const bool longReset = config_.useLongResetForRootSelection;
        const std::optional<uint8_t> gap = inputs.currentGapCount; // Explicitly preserve current gap

        snapshot_.selectedRoot = candidate->nodeId;
        snapshot_.requestedGapCount = inputs.currentGapCount;
        snapshot_.lastAction = longReset ? RootSelectionAction::ForceRootAndLongReset
                                         : RootSelectionAction::ForceRootAndShortReset;

        const bool ok = executor.ForceRootAndResetForBMPolicy(inputs.generation,
                                                              candidate->nodeId,
                                                              longReset,
                                                              gap);
        if (!ok) {
            snapshot_.lastDecision = RootSelectionDecision::FailedExecutorUnavailable;
            return;
        }

        attemptsThisStableTopology_++;
        snapshot_.attemptsThisTopology = attemptsThisStableTopology_;
        snapshot_.totalAttempts++;
        snapshot_.resetRequested = true;
        return;
    }

    case RootSelectionDecision::FailedRetryLimit:
        snapshot_.retryLimitHit = true;
        return;

    default:
        if (snapshot_.lastDecision != RootSelectionDecision::None &&
            snapshot_.lastDecision < RootSelectionDecision::SuppressedCycleAlreadyObserved) {
            snapshot_.suppressedCount++;
        }
        return;
    }
}

// IEEE 1394-2008 Annex H:
// If bus-management policy determines that the current root cannot provide
// cycle-start service, the active bus manager / fallback IRM may select a
// suitable cycle-master-capable node as root by PHY configuration followed by
// a bus reset. This is a bus-configuration action, not topology validation.
// Do not call this path for Annex P graph errors.
RootSelectionDecision RootSelectionCoordinator::Plan(const RootSelectionInputs& inputs) const noexcept {
    if (!inputs.topologyValid || inputs.topology == nullptr) {
        return RootSelectionDecision::SuppressedByTopology;
    }

    if (inputs.roleMode == RoleMode::ClientOnly) {
        return RootSelectionDecision::SuppressedByRoleMode;
    }

    if (inputs.activityLevel < FullBMActivityLevel::ForceRootAllowed) {
        return RootSelectionDecision::SuppressedByActivityLevel;
    }

    if (!IsAllowedActor(inputs)) {
        return RootSelectionDecision::SuppressedNotBMOrFallbackIRM;
    }

    if (inputs.cycleStartObserved) {
        return RootSelectionDecision::SuppressedCycleAlreadyObserved;
    }

    const auto* const currentRoot = FindNode(*inputs.topology, inputs.rootNodeId);
    if (currentRoot == nullptr) {
        return RootSelectionDecision::DeferredRootEvidenceIncomplete;
    }

    // Force-root decisions intentionally use only Self-ID evidence. Linux uses
    // BIB CMC for this branch; Apple uses contender/link from Self-ID, which is
    // the safer behavior for devices that advertise CMC=0 but still generate
    // valid cycle starts when root.
    // cross-validated with Linux: core-card.c:448-473 Apple: IOFireWireController.cpp:2364-2404
    if (currentRoot->linkActive && currentRoot->contender) {
        return RootSelectionDecision::SuppressedRootAlreadySuitable;
    }

    const auto candidate = SelectCandidate(inputs);
    if (!candidate.has_value()) {
        return RootSelectionDecision::FailedNoCandidate;
    }

    if (attemptsThisStableTopology_ >= config_.maxAttemptsPerStableTopology) {
        return RootSelectionDecision::FailedRetryLimit;
    }

    return candidate->isLocal ? RootSelectionDecision::SelectLocalRoot
                              : RootSelectionDecision::SelectRemoteRoot;
}

std::optional<RootCandidate>
RootSelectionCoordinator::SelectCandidate(const RootSelectionInputs& inputs) const noexcept {
    const auto candidates = BuildCandidates(inputs);
    if (candidates.empty()) {
        return std::nullopt;
    }

    // Sort by score descending
    auto best = candidates[0];
    for (size_t i = 1; i < candidates.size(); ++i) {
        if (candidates[i].score > best.score) {
            best = candidates[i];
        }
    }

    return best;
}

bool RootSelectionCoordinator::IsAllowedActor(const RootSelectionInputs& inputs) const noexcept {
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

uint32_t RootSelectionCoordinator::StableTopologyKey(const RootSelectionInputs& inputs) const noexcept {
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
        mix(node.contender ? 1u : 0u);
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

std::vector<RootCandidate>
RootSelectionCoordinator::BuildCandidates(const RootSelectionInputs& inputs) const {
    std::vector<RootCandidate> out;

    if (inputs.topology == nullptr) {
        return out;
    }

    const auto& topo = *inputs.topology;

    for (const auto& node : topo.physical.nodes) {
        // Skip link-inactive nodes
        if (!node.linkActive) {
            continue;
        }

        RootCandidate c{};
        c.nodeId = node.physicalId;
        c.isLocal = node.physicalId == inputs.localNodeId;
        c.isCurrentRoot = node.physicalId == inputs.rootNodeId;
        c.linkActive = node.linkActive;
        c.contender = node.contender;
        c.maxSpeedMbps = node.maxSpeedMbps;

        // If this node IS the current root, and current root was already rejected by Plan(),
        // don't consider it again unless Plan() thinks it's suitable (which it doesn't if we're here).
        if (c.isCurrentRoot) {
            continue;
        }

        if (c.isLocal) {
            c.transactionCapable = true;
            c.reason = RootCandidateReason::LocalSelfIDContender;
        } else {
            c.transactionCapable = node.linkActive;
            c.reason = RootCandidateReason::RemoteSelfIDContender;
        }

        if (!c.contender || !c.transactionCapable) {
            continue;
        }

        if (c.isLocal) {
            c.score += 100;
        } else {
            c.score += 80;
        }

        if (c.contender) {
            c.score += 10;
        }

        if (c.maxSpeedMbps >= 800) {
            c.score += 4;
        } else if (c.maxSpeedMbps >= 400) {
            c.score += 3;
        } else if (c.maxSpeedMbps >= 200) {
            c.score += 2;
        } else if (c.maxSpeedMbps >= 100) {
            c.score += 1;
        }

        out.push_back(c);
    }

    return out;
}

} // namespace ASFW::Bus
