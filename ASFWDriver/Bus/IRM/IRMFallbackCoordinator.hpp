// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2024 ASFireWire Project
//
// IRMFallbackCoordinator.hpp — IRM fallback detector and cycle policy planner (Milestone 4).

#pragma once

#include "../../Common/CSRSpace.hpp"
#include "../../Controller/ControllerConfig.hpp"
#include "../../Controller/ControllerTypes.hpp"
#include "../../Scheduling/Scheduler.hpp"
#include "../BusManager/BusManagerRuntimeState.hpp"
#include "../Timing/PostResetTiming.hpp"
#include "LocalCSRAccessor.hpp"
#include <cstdint>
#include <memory>

namespace ASFW::Bus {

namespace Timing {
class PostResetTimingCoordinator;
}

/**
 * @brief States for the IRM fallback state machine.
 */
enum class IRMFallbackState : uint8_t {
    Disabled = 0,
    WaitingForTopology = 1,
    NotLocalIRM = 2,
    WaitingForAnnexHGate = 3,
    ProbingBusManagerId = 4,
    BMExists = 5,
    NoBMDetected = 6,
    PlanningCycleRepair = 7,
    ActionSuppressedByPolicy = 8,
    SuppressedByTopology = 9,
    ProbeFailed = 10,
    StaleGeneration = 11,
};

/**
 * @brief Planned actions if no Bus Manager is detected after the fallback gate.
 */
enum class IRMFallbackAction : uint8_t {
    None = 0,
    DiagnosticsOnly = 1,                 // M4 default: report only
    LocalRootEnableCycleMasterRequired = 2,
    RemoteRootCmstrRequired = 3,
    RootSelectionRequired = 4,
    GapPolicyRequired = 5,
    BMAlreadyExists = 6,
    CycleStartAlreadyObserved = 7,
    SuppressedByRolePolicy = 8,
    SuppressedByActivityLevel = 9,
    SuppressedByTopology = 10,
};

/**
 * @brief Status of the BUS_MANAGER_ID probe.
 */
enum class BMIDProbeStatus : uint8_t {
    NotAttempted = 0,
    Success = 1,
    InvalidUpperBits = 2,
    HardwareUnavailable = 3,
    Timeout = 4,
};

/**
 * @brief Flat snapshot of the IRM fallback state for diagnostics.
 */
struct IRMFallbackSnapshot {
    IRMFallbackState state{IRMFallbackState::Disabled};
    IRMFallbackAction plannedAction{IRMFallbackAction::None};
    BMIDProbeStatus probeStatus{BMIDProbeStatus::NotAttempted};

    uint32_t generation{0};

    uint8_t localNodeId{0x3F};
    uint8_t irmNodeId{0x3F};
    uint8_t rootNodeId{0x3F};
    uint8_t bmNodeId{0x3F};

    bool roleAllowsIRMHost{false};
    bool localIsIRM{false};
    bool localIsRoot{false};
    bool topologyValid{false};

    bool annexHGateOpen{false};

    bool rootCmcKnown{false};
    bool rootCmcCapable{false};

    bool cycleStartObserved{false};
    uint8_t cycleStartSourceNode{0x3F};

    uint32_t busManagerIdRaw{0x0000003F};
    bool noBusManagerDetected{false};
    bool busManagerExists{false};

    uint64_t checkedAtNs{0};
    uint64_t allowedAtNs{0};
    uint64_t remainingNs{0};

    uint32_t staleGenerationDrops{0};
    uint32_t suppressedByPolicy{0};
    uint32_t probeFailures{0};
};

/**
 * @brief Coordinates IRM fallback detection and cycle repair planning (Milestone 4).
 *
 * This class implements the Annex H +625ms timing gate for IRM-led recovery.
 * It is strictly for diagnostics and planning in M4; no mutations are performed.
 */
class IRMFallbackCoordinator final : public std::enable_shared_from_this<IRMFallbackCoordinator> {
public:
    struct Deps {
        Driver::HardwareInterface& hardware;
        Timing::PostResetTimingCoordinator* timing{nullptr};
        Driver::Scheduler* scheduler{nullptr};
    };

    explicit IRMFallbackCoordinator(Deps deps) noexcept;
    ~IRMFallbackCoordinator() = default;

    // Disable copy/move
    IRMFallbackCoordinator(const IRMFallbackCoordinator&) = delete;
    IRMFallbackCoordinator& operator=(const IRMFallbackCoordinator&) = delete;

    /**
     * @brief Called when a bus reset begins.
     */
    void OnBusResetStarted(uint32_t generation) noexcept;

    /**
     * @brief Called when topology is accepted and role/state is known.
     */
    void OnTopologyReady(const Driver::TopologySnapshot& topology,
                         const Driver::RolePolicy& rolePolicy,
                         const BusManagerRuntimeState& bmState,
                         uint64_t nowNs) noexcept;

    /**
     * @brief Periodic or deferred evaluation point.
     */
    void MaybeEvaluate(uint64_t nowNs) noexcept;

    /**
     * @brief Disables the coordinator state.
     */
    void Disable() noexcept;

    /**
     * @brief Refreshes evidence (e.g. root CMC status, CycleStart observed) that
     * arrives after the topology has settled.
     */
    void OnRuntimeEvidenceUpdated(const BusManagerRuntimeState& bmState) noexcept;

    [[nodiscard]] const IRMFallbackSnapshot& Snapshot() const noexcept { return snapshot_; }

private:
    Deps deps_;
    IRMFallbackSnapshot snapshot_;
    LocalCSRAccessor csr_;

    [[nodiscard]] bool RoleAllowsFallbackCheck(const Driver::RolePolicy& rolePolicy) const noexcept;
    [[nodiscard]] BMIDProbeStatus ProbeBusManagerId(uint32_t* outValue) noexcept;
    [[nodiscard]] IRMFallbackAction PlanFallbackAction() const noexcept;
};

} // namespace ASFW::Bus
