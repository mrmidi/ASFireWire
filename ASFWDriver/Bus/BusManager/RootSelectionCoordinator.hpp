// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2024 ASFireWire Project
//
// RootSelectionCoordinator.hpp — Root selection and force-root policy (Milestone 6).

#pragma once

#include "../../Controller/ControllerConfig.hpp"
#include "../../Controller/ControllerTypes.hpp"
#include "../TopologyManager.hpp"
#include "BusManagerRuntimeState.hpp"
#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace ASFW::Bus {

/**
 * @brief Functional decision for root selection.
 */
enum class RootSelectionDecision : uint8_t {
    None = 0,

    SuppressedByRoleMode,
    SuppressedByActivityLevel,
    SuppressedByTopology,
    SuppressedNotBMOrFallbackIRM,
    SuppressedCycleAlreadyObserved,
    SuppressedRootAlreadySuitable,
    DeferredRootEvidenceIncomplete,
    DeferredCandidateEvidenceIncomplete,

    SelectLocalRoot,
    SelectRemoteRoot,

    FailedNoCandidate,
    FailedRetryLimit,
    FailedGenerationStale,
    FailedExecutorUnavailable,
};

/**
 * @brief Execution action for root selection.
 */
enum class RootSelectionAction : uint8_t {
    None = 0,
    ForceRootAndShortReset,
    ForceRootAndLongReset,
    ReportOnly,
};

/**
 * @brief Reason for choosing a root candidate.
 */
enum class RootCandidateReason : uint8_t {
    None = 0,
    LocalSelfIDContender,
    RemoteSelfIDContender,
    CurrentRootSelfIDContender,
    MissingSelfIDContender,
};

/**
 * @brief Represents a viable root node candidate.
 */
struct RootCandidate {
    uint8_t nodeId{0x3F};

    bool isLocal{false};
    bool isCurrentRoot{false};
    bool linkActive{false};
    bool transactionCapable{false};

    bool contender{false};
    uint32_t maxSpeedMbps{0};

    uint8_t score{0};
    RootCandidateReason reason{RootCandidateReason::None};
};

/**
 * @brief Inputs for root selection planning.
 */
struct RootSelectionInputs {
    uint32_t generation{0};
    uint16_t busBase16{0x03FF};

    ASFW::FW::RoleMode roleMode{ASFW::FW::RoleMode::ClientOnly};
    ASFW::FW::FullBMActivityLevel activityLevel{ASFW::FW::FullBMActivityLevel::ObserveOnly};

    bool topologyValid{false};
    uint8_t localNodeId{0x3F};
    uint8_t rootNodeId{0x3F};
    uint8_t irmNodeId{0x3F};
    uint8_t bmNodeId{0x3F};

    bool localIsRoot{false};
    bool localIsIRM{false};
    bool localIsBM{false};

    bool irmFallbackGateOpen{false};
    bool irmFallbackNoBMDetected{false};

    bool cycleStartObserved{false};

    uint8_t currentGapCount{63};

    const ASFW::Driver::TopologySnapshot* topology{nullptr};
};

/**
 * @brief Snapshot of the root selection state for diagnostics.
 */
struct RootSelectionSnapshot {
    uint32_t generation{0};

    RootSelectionDecision lastDecision{RootSelectionDecision::None};
    RootSelectionAction lastAction{RootSelectionAction::None};

    uint8_t selectedRoot{0x3F};
    uint8_t previousRoot{0x3F};

    uint8_t currentGapCount{63};
    uint8_t requestedGapCount{0};

    uint32_t attemptsThisTopology{0};
    uint32_t totalAttempts{0};
    uint32_t suppressedCount{0};
    uint32_t staleGenerationDrops{0};

    bool resetRequested{false};
    bool retryLimitHit{false};
};

/**
 * @brief Configuration for root selection.
 */
struct RootSelectionConfig {
    uint32_t maxAttemptsPerStableTopology{5};
    bool useLongResetForRootSelection{false};
    bool preferLocalRootWhenContender{true};
    bool allowRemoteRootSelection{true};
};

/**
 * @brief Executor interface for root selection mutations.
 */
struct IRootSelectionExecutor {
    virtual ~IRootSelectionExecutor() = default;

    /**
     * @brief Forces a specific node to be root and triggers a bus reset.
     * M6 passes the current gap count through unmodified.
     */
    virtual bool ForceRootAndResetForBMPolicy(uint32_t generation,
                                              uint8_t targetRoot,
                                              bool longReset,
                                              std::optional<uint8_t> gapCount) = 0;
};

/**
 * @brief Coordinates root selection and force-root policy (Milestone 6).
 *
 * This class selects a suitable Self-ID contender/link-active node as root and
 * forces a bus reset when the current root is unsuitable. It implements strict
 * retry limits per stable topology to prevent reset storms.
 */
class RootSelectionCoordinator final {
public:
    explicit RootSelectionCoordinator(RootSelectionConfig config) noexcept;
    ~RootSelectionCoordinator() = default;

    // Disable copy/move
    RootSelectionCoordinator(const RootSelectionCoordinator&) = delete;
    RootSelectionCoordinator& operator=(const RootSelectionCoordinator&) = delete;

    /**
     * @brief Resets generation-scoped state.
     */
    void OnBusResetStarted(uint32_t generation) noexcept;

    /**
     * @brief Evaluates current bus state and performs root mutation if needed.
     */
    void Evaluate(const RootSelectionInputs& inputs,
                  IRootSelectionExecutor& executor) noexcept;

    /**
     * @brief Pure planner logic for testing.
     */
    [[nodiscard]] RootSelectionDecision Plan(const RootSelectionInputs& inputs) const noexcept;

    /**
     * @brief Selects the best candidate from the current topology.
     */
    [[nodiscard]] std::optional<RootCandidate>
    SelectCandidate(const RootSelectionInputs& inputs) const noexcept;

    [[nodiscard]] const RootSelectionSnapshot& Snapshot() const noexcept {
        return snapshot_;
    }

private:
    [[nodiscard]] bool IsAllowedActor(const RootSelectionInputs& inputs) const noexcept;
    [[nodiscard]] uint32_t StableTopologyKey(const RootSelectionInputs& inputs) const noexcept;
    [[nodiscard]] std::vector<RootCandidate>
    BuildCandidates(const RootSelectionInputs& inputs) const;

    RootSelectionConfig config_{};
    RootSelectionSnapshot snapshot_{};

    uint32_t stableTopologyKey_{0};
    uint32_t attemptsThisStableTopology_{0};
};

} // namespace ASFW::Bus
