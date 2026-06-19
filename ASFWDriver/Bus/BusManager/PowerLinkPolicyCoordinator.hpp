// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2024 ASFireWire Project
//
// PowerLinkPolicyCoordinator.hpp — Power management and Link-On policy (Milestone 8).

#pragma once

#include "../../Controller/ControllerConfig.hpp"
#include "../../Controller/ControllerTypes.hpp"
#include "../TopologyManager.hpp"
#include <array>
#include <cstdint>
#include <vector>

namespace ASFW::Bus {

/**
 * @brief Functional decision for power/link policy.
 */
enum class PowerPolicyDecision : uint8_t {
    None = 0,

    SuppressedByRoleMode,
    SuppressedByPolicyLevel,
    SuppressedByTopology,
    SuppressedNotBMOrFallbackIRM,

    NoEligibleNodes,
    DeferredPowerBudgetUnknown,
    DeferredInsufficientPower,
    DeferredNodeEvidenceIncomplete,

    LinkOnRequired,
    LinkOnAlreadyAttemptedThisGeneration,

    FailedRetryLimit,
    FailedExecutorUnavailable,
    FailedGenerationStale,
};

/**
 * @brief Execution action for power/link policy.
 */
enum class PowerPolicyAction : uint8_t {
    None = 0,
    ReportOnly,
    SendLinkOnPackets,
};

/**
 * @brief Status of the bus power budget.
 */
enum class PowerBudgetStatus : uint8_t {
    Unknown = 0,
    Sufficient = 1,
    Insufficient = 2,
};

/**
 * @brief Conservative power budget estimate from Self-ID pwr fields.
 */
struct PowerBudgetEstimate {
    PowerBudgetStatus status{PowerBudgetStatus::Unknown};
    uint32_t availableMilliWatts{0};
    uint32_t requiredMilliWatts{0};
    uint32_t unknownPowerClassNodes{0};
};

/**
 * @brief Reason for targeting a node for Link-On.
 */
enum class LinkOnTargetReason : uint8_t {
    None = 0,
    LinkInactiveSelfID,     ///< L-bit is 0 in Self-ID.
    LinkOffButPhyPresent,  ///< PHY present but link layer inactive.
};

/**
 * @brief Evidence gathered for a potential Link-On target.
 * cross-validated with Linux: core-topology.c:26-36.
 */
struct PowerLinkNodeEvidence {
    uint8_t nodeId{0x3F};

    bool isLocal{false};
    bool isRoot{false};
    bool linkActive{false};
    bool phyPresent{false};

    uint8_t powerClass{0};
    bool powerClassKnown{false};

    bool eligibleForLinkOn{false};
    LinkOnTargetReason reason{LinkOnTargetReason::None};
};

/**
 * @brief Inputs for power/link policy planning.
 */
struct PowerLinkPolicyInputs {
    uint32_t generation{0};
    uint16_t busBase16{0xFFC0};

    ASFW::FW::RoleMode roleMode{ASFW::FW::RoleMode::ClientOnly};
    Driver::PowerPolicyLevel powerPolicyLevel{Driver::PowerPolicyLevel::ObserveOnly};

    bool topologyValid{false};

    uint8_t localNodeId{0x3F};
    uint8_t rootNodeId{0x3F};
    uint8_t irmNodeId{0x3F};
    uint8_t bmNodeId{0x3F};

    bool localIsBM{false};
    bool localIsIRM{false};

    bool irmFallbackGateOpen{false};
    bool irmFallbackNoBMDetected{false};

    PowerBudgetStatus powerBudgetStatus{PowerBudgetStatus::Unknown};

    const ASFW::Driver::TopologySnapshot* topology{nullptr};
};

/**
 * @brief Snapshot of the power/link policy state for diagnostics.
 */
struct PowerLinkPolicySnapshot {
    uint32_t generation{0};

    PowerPolicyDecision lastDecision{PowerPolicyDecision::None};
    PowerPolicyAction lastAction{PowerPolicyAction::None};

    PowerBudgetStatus powerBudgetStatus{PowerBudgetStatus::Unknown};

    uint32_t eligibleNodeCount{0};
    uint32_t powerAvailableMilliWatts{0};
    uint32_t powerRequiredMilliWatts{0};
    uint32_t unknownPowerClassNodes{0};

    uint32_t linkOnSubmittedCount{0};
    uint32_t linkOnSuccessCount{0};
    uint32_t linkOnFailureCount{0};

    uint32_t attemptsThisGeneration{0};
    uint32_t totalAttempts{0};
    uint32_t suppressedCount{0};
    uint32_t staleGenerationDrops{0};

    std::array<uint8_t, 16> targetNodes{};
    uint8_t targetNodeCount{0};
};

/**
 * @brief Configuration for power/link policy.
 */
struct PowerLinkPolicyConfig {
    uint32_t maxLinkOnAttemptsPerGeneration{1};

    bool requireKnownPowerBudget{true};
    bool allowWhenPowerBudgetUnknown{false};

    bool skipRootNode{true};
    bool skipLocalNode{true};
    bool allowPhyOnlyTargets{false};

    uint32_t maxTargetsPerEvaluation{16};
};

/**
 * @brief Executor interface for Link-On packets.
 */
struct ILinkOnExecutor {
    virtual ~ILinkOnExecutor() = default;

    /**
     * @brief Sends a Link-On PHY packet to the target node.
     * cross-validated with Linux: core-cdev.c:1624-1640.
     */
    virtual bool SendLinkOnPacket(uint32_t generation,
                                  uint16_t busBase16,
                                  uint8_t targetNodeId) = 0;
};

/**
 * @brief Coordinates power management and Link-On policy (Milestone 8).
 *
 * This class identifies nodes with inactive link layers and decides whether
 * to wake them with Link-On packets. It is strictly gated by PowerPolicyLevel
 * and actor roles (BM or fallback IRM).
 */
class PowerLinkPolicyCoordinator final {
public:
    explicit PowerLinkPolicyCoordinator(PowerLinkPolicyConfig config) noexcept;
    ~PowerLinkPolicyCoordinator() = default;

    // Disable copy/move
    PowerLinkPolicyCoordinator(const PowerLinkPolicyCoordinator&) = delete;
    PowerLinkPolicyCoordinator& operator=(const PowerLinkPolicyCoordinator&) = delete;

    /**
     * @brief Resets generation-scoped state.
     */
    void OnBusResetStarted(uint32_t generation) noexcept;

    /**
     * @brief Evaluates current bus state and performs Link-On if needed.
     */
    void Evaluate(const PowerLinkPolicyInputs& inputs,
                  ILinkOnExecutor& executor) noexcept;

    /**
     * @brief Pure planner logic for testing.
     */
    [[nodiscard]] PowerPolicyDecision Plan(const PowerLinkPolicyInputs& inputs,
                                           const std::vector<PowerLinkNodeEvidence>& candidates) const noexcept;

    /**
     * @brief Builds a list of link-inactive candidates from the topology.
     */
    [[nodiscard]] std::vector<PowerLinkNodeEvidence>
    BuildCandidates(const PowerLinkPolicyInputs& inputs) const;

    /**
     * @brief Computes a conservative bus power budget from Self-ID pwr fields.
     */
    [[nodiscard]] PowerBudgetEstimate
    EstimatePowerBudget(const PowerLinkPolicyInputs& inputs) const noexcept;

    [[nodiscard]] const PowerLinkPolicySnapshot& Snapshot() const noexcept {
        return snapshot_;
    }

private:
    [[nodiscard]] bool IsAllowedActor(const PowerLinkPolicyInputs& inputs) const noexcept;
    [[nodiscard]] PowerPolicyDecision BudgetDecision(PowerBudgetStatus status) const noexcept;

    PowerLinkPolicyConfig config_{};
    PowerLinkPolicySnapshot snapshot_{};
};

} // namespace ASFW::Bus
