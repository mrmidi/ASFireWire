// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2024 ASFireWire Project
//
// CyclePolicyCoordinator.hpp — Cycle start generation policy and repair planner (Milestone 5).

#pragma once

#include "../../Common/CSRSpace.hpp"
#include "../../Controller/ControllerConfig.hpp"
#include "../../Controller/ControllerTypes.hpp"
#include "BusManagerRuntimeState.hpp"
#include "../../Async/Interfaces/IAsyncControllerPort.hpp"
#include <cstdint>
#include <memory>

namespace ASFW::Bus {

/**
 * @brief Executor interface for cycle policy mutations.
 */
struct ICyclePolicyExecutor {
    virtual ~ICyclePolicyExecutor() = default;

    /**
     * @brief Enables the local OHCI cycle master.
     * Returns true if success verified via readback.
     */
    virtual bool EnableLocalCycleMasterMutation(uint32_t generation) = 0;

    /**
     * @brief Clears the local OHCI cycle master when the local node is not root.
     * Returns true if success verified via readback.
     */
    virtual bool ClearLocalCycleMasterMutation(uint32_t generation) = 0;

    /**
     * @brief Sends a remote STATE_SET.cmstr write to the target node.
     */
    virtual Async::AsyncHandle WriteRemoteStateSetCmstr(uint32_t generation,
                                                        uint16_t busBase16,
                                                        uint8_t targetNodeId) = 0;
};

/**
 * @brief Functional decision for cycle repair.
 */
enum class CyclePolicyDecision : uint8_t {
    None = 0,

    SuppressedByRoleMode,
    SuppressedByActivityLevel,
    SuppressedByTopology,
    SuppressedByGeneration,
    SuppressedNotBMOrFallbackIRM,

    AlreadySatisfiedCycleStartObserved,
    AlreadySatisfiedLocalCycleMasterEnabled,

    LocalCycleMasterClearNotRoot,

    DeferRootSelfIDUnknown,
    DeferLocalSelfIDUnknown,

    LocalRootEnableCycleMaster,
    RemoteRootSetCmstr,
    RootSelectionRequired,

    FailedHardwareUnavailable,
    FailedAsyncSubmit,
    FailedGenerationStale,
    DeferRootBibCmcUnknown,
};

/**
 * @brief Execution action for the cycle policy.
 */
enum class CyclePolicyAction : uint8_t {
    None = 0,
    EnableLocalCycleMaster,
    ClearLocalCycleMaster,
    WriteRemoteStateSetCmstr,
    ReportRootSelectionRequired,
};

/**
 * @brief Inputs for cycle policy planning.
 */
struct CyclePolicyInputs {
    uint32_t generation{0};
    uint16_t busBase16{0x03FF};

    uint8_t localNodeId{0x3F};
    uint8_t rootNodeId{0x3F};
    uint8_t irmNodeId{0x3F};
    uint8_t bmNodeId{0x3F};

    bool topologyValid{false};

    bool localIsRoot{false};
    bool localIsIRM{false};
    bool localIsBM{false};
    bool localCycleMasterEnabled{false};

    bool irmFallbackNoBMDetected{false};
    bool irmFallbackGateOpen{false};

    bool cycleStartObserved{false};
    uint8_t cycleStartSourceNode{0x3F};

    bool rootSelfIdKnown{false};
    bool rootSelfIdLinkActive{false};
    bool rootSelfIdContender{false};

    bool localSelfIdKnown{false};
    bool localSelfIdLinkActive{false};
    bool localSelfIdContender{false};

    // BIB CMC evidence gates only remote STATE_SET.cmstr. It must not drive
    // root selection because some devices report CMC=0 while their
    // Self-ID/behavior proves they can host cycle services.
    bool rootCmcKnown{false};
    bool rootCmcCapable{false};
    bool localCmcKnown{false};
    bool localCmcCapable{false};

    ASFW::FW::RoleMode roleMode{ASFW::FW::RoleMode::ClientOnly};
    ASFW::FW::FullBMActivityLevel activityLevel{ASFW::FW::FullBMActivityLevel::ObserveOnly};
};

/**
 * @brief Snapshot of the cycle policy state for diagnostics.
 */
struct CyclePolicySnapshot {
    uint32_t generation{0};

    CyclePolicyDecision lastDecision{CyclePolicyDecision::None};
    CyclePolicyAction lastAction{CyclePolicyAction::None};

    uint8_t targetNode{0x3F};

    bool localCycleMasterBefore{false};
    bool localCycleMasterAfter{false};

    bool remoteCmstrInFlight{false};
    uint32_t remoteCmstrGeneration{0};
    uint8_t remoteCmstrTargetNode{0x3F};
    uint8_t remoteCmstrStatus{0}; // ASFW::Async::AsyncStatus

    uint32_t localCycleMasterEnableCount{0};
    uint32_t localCycleMasterClearCount{0};
    uint32_t remoteCmstrSubmitCount{0};
    uint32_t suppressedCount{0};
    uint32_t staleGenerationDrops{0};
};

/**
 * @brief Coordinates cycle-start generation policy (Milestone 5).
 *
 * This class decides whether to enable local cycleMaster or send remote CMSTR writes
 * based on BM/IRM status and cycle evidence.
 */
class CyclePolicyCoordinator final {
public:
    CyclePolicyCoordinator() noexcept = default;
    ~CyclePolicyCoordinator() = default;

    // Disable copy/move
    CyclePolicyCoordinator(const CyclePolicyCoordinator&) = delete;
    CyclePolicyCoordinator& operator=(const CyclePolicyCoordinator&) = delete;

    /**
     * @brief Evaluates current bus state, returns a decision, and performs mutation if needed.
     */
    void Evaluate(const CyclePolicyInputs& inputs, ICyclePolicyExecutor& executor) noexcept;

    /**
     * @brief Pure planner logic for testing.
     */
    [[nodiscard]] CyclePolicyDecision Plan(const CyclePolicyInputs& inputs) const noexcept;

    [[nodiscard]] const CyclePolicySnapshot& Snapshot() const noexcept { return snapshot_; }

    /**
     * @brief Resets the snapshot state for a new generation.
     */
    void OnBusResetStarted(uint32_t generation) noexcept;

    /**
     * @brief Completion callback for remote CMSTR write.
     */
    void OnRemoteCmstrComplete(uint32_t generation, uint8_t targetNode,
                               Async::AsyncStatus status) noexcept;

private:
    CyclePolicySnapshot snapshot_{};

    uint32_t lastLocalCycleMasterGeneration_{0};
    uint32_t lastRemoteCmstrGeneration_{0};
    uint8_t lastRemoteCmstrTargetNode_{0x3F};
    Async::AsyncHandle remoteCmstrHandle_{};
};

} // namespace ASFW::Bus
