// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#pragma once

#include "AVCUnit.hpp"
#include "IAVCDiscovery.hpp"
#include "RemoteAvcSession.hpp"
#include "../Ports/FireWireBusPort.hpp"
#include "../../Discovery/IDeviceManager.hpp"

#include <DriverKit/IOLib.h>

#include <atomic>
#include <map>
#include <memory>
#include <unordered_map>
#include <vector>

namespace ASFW::Protocols::AVC {

// Standards-only AV/C unit and per-device FCP registry. It never selects an
// audio family, creates an audio profile, or publishes an audio nub.
class AVCDiscovery final : public Discovery::IUnitObserver,
                           public IAVCDiscovery {
public:
    AVCDiscovery(Discovery::DeviceRegistry& deviceRegistry,
                 Discovery::IDeviceManager& deviceManager,
                 Protocols::Ports::FireWireBusOps& busOps,
                 Protocols::Ports::FireWireBusInfo& busInfo,
                 Scheduling::ITimerScheduler& timerScheduler);
    ~AVCDiscovery() override;

    AVCDiscovery(const AVCDiscovery&) = delete;
    AVCDiscovery& operator=(const AVCDiscovery&) = delete;

    void OnUnitPublished(std::shared_ptr<Discovery::FWUnit> unit) override;
    void OnUnitSuspended(std::shared_ptr<Discovery::FWUnit> unit) override;
    void OnUnitResumed(std::shared_ptr<Discovery::FWUnit> unit) override;
    void OnUnitTerminated(std::shared_ptr<Discovery::FWUnit> unit) override;

    [[nodiscard]] std::shared_ptr<AVCUnit> AcquireAVCUnit(
        Discovery::UnitInstanceId unitId) noexcept;
    std::vector<AVCUnit*> GetAllAVCUnits() override;
    std::shared_ptr<RemoteAvcSession> AcquireRemoteSession(
        Discovery::UnitInstanceId unitId) override;
    void ReScanAllUnits() override;

    std::shared_ptr<FCPTransport> AcquireFCPTransportForIngress(
        Discovery::Generation generation, uint16_t sourceId) override;

    void OnBusReset(uint32_t newGeneration);
    void Shutdown();

private:
    [[nodiscard]] bool IsAVCUnit(
        const std::shared_ptr<Discovery::FWUnit>& unit) const noexcept;
    [[nodiscard]] std::shared_ptr<FCPTransport> EnsureRemoteTransportLocked(
        const std::shared_ptr<Discovery::FWDevice>& device);

    Discovery::DeviceRegistry& deviceRegistry_;
    Discovery::IDeviceManager& deviceManager_;
    Protocols::Ports::FireWireBusOps& busOps_;
    Protocols::Ports::FireWireBusInfo& busInfo_;
    Scheduling::ITimerScheduler& timerScheduler_;
    IOLock* lock_{nullptr};
    std::map<Discovery::UnitInstanceId, std::shared_ptr<AVCUnit>> units_;
    std::unordered_map<Discovery::DeviceInstanceId,
                       std::shared_ptr<FCPTransport>,
                       Discovery::DeviceInstanceIdHash> transportsByDevice_;
    std::atomic<bool> shuttingDown_{false};
};

} // namespace ASFW::Protocols::AVC
