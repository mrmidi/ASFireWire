// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2024 ASFireWire Project
//
// GapPolicyCoordinator.hpp — Gap count optimization policy and planner (Milestone 7).

#pragma once

#include "../../Controller/ControllerConfig.hpp"
#include "../../Controller/ControllerTypes.hpp"
#include "../TopologyManager.hpp"
#include <array>
#include <cstdint>
#include <optional>

namespace ASFW::Bus {

/**
 * @brief Functional decision for gap count optimization.
 */
enum class GapPolicyDecision : uint8_t {
    None = 0,

    SuppressedByRoleMode,
    SuppressedByActivityLevel,
    SuppressedByTopology,
    SuppressedNotBMOrFallbackIRM,
    SuppressedSingleNodeBus,

    DeferMaxHopsUnavailable,
    DeferBetaRepeaterUnknown,

    AlreadyOptimal,
    GapMismatchRequiresLongReset,
    GapOptimizationRequired,

    FailedRetryLimit,
    FailedExecutorUnavailable,
    FailedGenerationStale,
};

/**
 * @brief Execution action for the gap policy.
 */
enum class GapPolicyAction : uint8_t {
    None = 0,
    ReportOnly,
    ForceRootWithGapAndShortReset,
    ForceRootWithGapAndLongReset,
    GapOnlyShortReset,
    GapOnlyLongReset,
};

/**
 * @brief Strategy used to compute the expected gap count.
 */
enum class GapComputationSource : uint8_t {
    None = 0,
    Table1394a,
    DefaultSafe63,
    ExistingGapPreserved,
};

/**
 * @brief Configuration for gap policy.
 */
struct GapPolicyConfig {
    bool enable1394aTable{true};
    bool optimizeWhenBetaRepeatersUnknown{false};
    bool useLongResetForGapMismatch{true};
    bool useShortResetForPureOptimization{true};

    uint32_t maxOptimizationAttemptsPerStableTopology{5};
    uint32_t maxMismatchRepairsPerStableTopology{2};
};

/**
 * @brief Inputs for gap policy planning.
 */
struct GapPolicyInputs {
    uint32_t generation{0};

    ASFW::FW::RoleMode roleMode{ASFW::FW::RoleMode::ClientOnly};
    ASFW::FW::FullBMActivityLevel activityLevel{ASFW::FW::FullBMActivityLevel::ObserveOnly};

    bool topologyValid{false};

    uint8_t localNodeId{0x3F};
    uint8_t rootNodeId{0x3F};
    uint8_t irmNodeId{0x3F};
    uint8_t bmNodeId{0x3F};

    bool localIsBM{false};
    bool localIsIRM{false};

    bool irmFallbackGateOpen{false};
    bool irmFallbackNoBMDetected{false};

    uint8_t currentGapCount{63};
    bool gapCountConsistent{true};

    bool maxHopsKnown{false};
    uint8_t maxHopsFromRoot{0};

    bool betaRepeatersKnown{false};
    bool betaRepeatersPresent{false};

    bool rootSelectionRequired{false};
    uint8_t selectedRootForRootPolicy{0x3F};

    const ASFW::Driver::TopologySnapshot* topology{nullptr};
};

/**
 * @brief Snapshot of the gap policy state for diagnostics.
 */
struct GapPolicySnapshot {
    uint32_t generation{0};

    GapPolicyDecision lastDecision{GapPolicyDecision::None};
    GapPolicyAction lastAction{GapPolicyAction::None};
    GapComputationSource computationSource{GapComputationSource::None};

    uint8_t currentGapCount{63};
    uint8_t expectedGapCount{63};
    uint8_t requestedGapCount{0};

    uint8_t maxHopsFromRoot{0};
    bool maxHopsKnown{false};

    bool gapCountConsistent{true};
    bool betaRepeatersKnown{false};
    bool betaRepeatersPresent{false};

    bool resetRequested{false};
    bool combinedWithRootSelection{false};

    uint8_t targetRoot{0x3F};

    uint32_t attemptsThisTopology{0};
    uint32_t totalAttempts{0};
    uint32_t mismatchRepairsThisTopology{0};
    uint32_t retryLimitHit{0};
    uint32_t suppressedCount{0};
};

/**
 * @brief Executor interface for gap policy mutations.
 */
struct IGapPolicyExecutor {
    virtual ~IGapPolicyExecutor() = default;

    /**
     * @brief Forces a specific node to be root and sets a target gap count.
     */
    virtual bool ForceRootAndGapResetForBMPolicy(uint32_t generation,
                                                 uint8_t targetRoot,
                                                 bool longReset,
                                                 uint8_t gapCount) = 0;
};

/**
 * @brief Coordinates gap count optimization policy (Milestone 7).
 *
 * This class computes the optimal gap count for the current topology and
 * requests a combined PHY configuration reset when needed.
 */
class GapPolicyCoordinator final {
public:
    explicit GapPolicyCoordinator(GapPolicyConfig config) noexcept;
    ~GapPolicyCoordinator() = default;

    // Disable copy/move
    GapPolicyCoordinator(const GapPolicyCoordinator&) = delete;
    GapPolicyCoordinator& operator=(const GapPolicyCoordinator&) = delete;

    /**
     * @brief Resets generation-scoped state.
     */
    void OnBusResetStarted(uint32_t generation) noexcept;

    /**
     * @brief Evaluates current bus state and performs gap mutation if needed.
     */
    void Evaluate(const GapPolicyInputs& inputs,
                  IGapPolicyExecutor& executor) noexcept;

    /**
     * @brief Pure planner logic for testing.
     */
    [[nodiscard]] GapPolicyDecision Plan(const GapPolicyInputs& inputs) const noexcept;

    /**
     * @brief Computes the target gap count based on hops and beta repeaters.
     */
    [[nodiscard]] uint8_t ComputeExpectedGapCount(const GapPolicyInputs& inputs,
                                                  GapComputationSource* source) const noexcept;

    [[nodiscard]] const GapPolicySnapshot& Snapshot() const noexcept {
        return snapshot_;
    }

private:
    [[nodiscard]] bool IsAllowedActor(const GapPolicyInputs& inputs) const noexcept;
    [[nodiscard]] uint32_t StableTopologyKey(const GapPolicyInputs& inputs) const noexcept;

    GapPolicyConfig config_{};
    GapPolicySnapshot snapshot_{};

    uint32_t stableTopologyKey_{0};
    uint32_t attemptsThisStableTopology_{0};
    uint32_t mismatchRepairsThisStableTopology_{0};
};

} // namespace ASFW::Bus
