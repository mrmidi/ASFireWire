// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2024 ASFireWire Project
//
// BusManagerElectionDriver.hpp — Coordinates IEEE 1394 Bus Manager election (FW-18).

#pragma once

#include "../../Common/CSRSpace.hpp"
#include "../../Async/AsyncTypes.hpp"
#include "../../Async/Interfaces/IAsyncControllerPort.hpp"
#include "../../Controller/ControllerTypes.hpp"
#include "../CSR/CSRResponder.hpp"
#include "../../Controller/ControllerConfig.hpp"
#include "../../Scheduling/Scheduler.hpp"
#include "BusManagerElection.hpp"

#include <memory>

namespace ASFW::Driver {
class HardwareInterface;
}

namespace ASFW::Bus {

namespace Timing {
class PostResetTimingCoordinator;
}

class LocalIRMResourceController;

class BusManagerElectionDriver : public std::enable_shared_from_this<BusManagerElectionDriver> {
public:
    struct IBMRoleEvents {
        virtual ~IBMRoleEvents() = default;
        virtual void OnLocalWonBM(uint32_t generation, uint8_t localNodeId) = 0;
        virtual void OnRemoteBM(uint32_t generation, uint8_t remoteNodeId) = 0;
        virtual void OnBMElectionFailed(uint32_t generation, ASFW::Async::AsyncStatus status) = 0;
    };

    struct Deps {
        ASFW::Async::IAsyncControllerPort* asyncController{nullptr};
        ASFW::Driver::Scheduler* scheduler{nullptr};
        ASFW::Bus::CSRResponder* csrResponder{nullptr};
        ASFW::Driver::HardwareInterface* hardware{nullptr};
        LocalIRMResourceController* localIrmController{nullptr};
        Timing::PostResetTimingCoordinator* timing{nullptr};
    };

    BusManagerElectionDriver(Deps deps, ASFW::Driver::RolePolicy rolePolicy) noexcept;
    ~BusManagerElectionDriver() = default;

    void OnTopologyReady(const ASFW::Driver::TopologySnapshot& snap, uint64_t nowNs) noexcept;
    void OnBusReset() noexcept;
    void Stop() noexcept;

    void SetRolePolicy(const ASFW::Driver::RolePolicy& policy) noexcept { rolePolicy_ = policy; }
    void SetObserver(IBMRoleEvents* observer) noexcept { observer_ = observer; }

    // Accessors for diagnostics / testing
    [[nodiscard]] const BusManagerElection& FSM() const noexcept { return fsm_; }
    [[nodiscard]] BusManagerElection& FSM() noexcept { return fsm_; }

    [[nodiscard]] bool WasIncumbent() const noexcept { return wasIncumbent_; }
    [[nodiscard]] bool InFlight() const noexcept { return inFlight_; }
    [[nodiscard]] uint32_t InFlightGen() const noexcept { return inFlightGen_; }

private:
    void Contend(uint32_t generation, uint8_t localNodeId, uint8_t irmNodeId, uint16_t busBase16) noexcept;
    void HandleCompareSwapResult(uint32_t generation, uint8_t localNodeId, ASFW::Async::AsyncStatus status, uint32_t oldValue, bool compareMatched) noexcept;

    Deps deps_;
    ASFW::Driver::RolePolicy rolePolicy_;
    BusManagerElection fsm_;
    IBMRoleEvents* observer_{nullptr};
    ASFW::Async::AsyncHandle inFlightHandle_{};
    bool wasIncumbent_{false};
    bool inFlight_{false};
    uint32_t inFlightGen_{0};
    uint32_t attemptedGeneration_{0};
    uint8_t attemptsThisGeneration_{0};
    bool active_{true};
};

} // namespace ASFW::Bus
