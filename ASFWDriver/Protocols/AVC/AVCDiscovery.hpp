//
// AVCDiscovery.hpp
// ASFWDriver - AV/C Protocol Layer
//
// AV/C Discovery - auto-detects AV/C units and creates AVCUnit instances
// Implements IUnitObserver for lifecycle notifications
//

#pragma once

#include <DriverKit/IODispatchQueue.h>
#include <DriverKit/IOLib.h>
#include <DriverKit/OSSharedPtr.h>
#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include "IAVCDiscovery.hpp"
#include "AVCUnit.hpp"
#include "../Ports/FireWireBusPort.hpp"
#include "../../Discovery/IDeviceManager.hpp"
#include "../../Discovery/DeviceRouteToken.hpp"
#include "../../Discovery/FWUnit.hpp"
#include "../../Discovery/FWDevice.hpp"
#include "Discovery/DiscoveryOwner.hpp"
#include "../../Scheduling/ITimerScheduler.hpp"


// Forward declarations
namespace ASFW::Discovery { class DeviceRegistry; struct DeviceRecord; }
namespace ASFW::Audio::Model { struct ASFWAudioDevice; }
namespace ASFW::Protocols::AVC::Music { class MusicSubunit; }

namespace ASFW::Protocols::AVC {

//==============================================================================
// AV/C Discovery
//==============================================================================

class AVCDiscovery : public Discovery::IUnitObserver,
                     public Discovery::IDeviceObserver,
                     public IAVCDiscovery,
                     public std::enable_shared_from_this<AVCDiscovery> {
public:
    AVCDiscovery(IOService* driver,
                 Discovery::DeviceRegistry& deviceRegistry,
                 Discovery::IDeviceManager& deviceManager,
                 Protocols::Ports::FireWireBusOps& busOps,
                 Protocols::Ports::FireWireBusInfo& busInfo,
                 Scheduling::ITimerScheduler& timerScheduler,
                 std::shared_ptr<DiscoveryOwner> owner);

    ~AVCDiscovery() override;

    AVCDiscovery(const AVCDiscovery&) = delete("discovery owns every AV/C unit on the bus; there is exactly one");
    AVCDiscovery& operator=(const AVCDiscovery&) = delete("discovery owns every AV/C unit on the bus; there is exactly one");

    void OnUnitPublished(std::shared_ptr<Discovery::FWUnit> unit) override;
    void OnUnitSuspended(std::shared_ptr<Discovery::FWUnit> unit) override;
    void OnUnitResumed(std::shared_ptr<Discovery::FWUnit> unit) override;
    void OnUnitTerminated(std::shared_ptr<Discovery::FWUnit> unit) override;
    void OnDeviceAdded(std::shared_ptr<Discovery::FWDevice> device) override;
    void OnDeviceResumed(std::shared_ptr<Discovery::FWDevice> device) override;
    void OnDeviceSuspended(std::shared_ptr<Discovery::FWDevice> device) override;
    void OnDeviceRemoved(Discovery::Guid64 guid) override;

    AVCUnit* GetAVCUnit(uint64_t guid);

    AVCUnit* GetAVCUnit(std::shared_ptr<Discovery::FWUnit> unit);

    std::vector<AVCUnit*> GetAllAVCUnits() override;

    void ReScanAllUnits() override;

    /// Stop every FCP producer before the async subsystem is dismantled.
    void Shutdown();

    FCPTransport* GetFCPTransportForNodeID(uint16_t nodeID) override;

    std::shared_ptr<FCPTransport> AcquireFCPTransportForNodeID(uint16_t nodeID) override;

    void OnBusReset(uint32_t newGeneration);

private:
    bool IsAVCUnit(std::shared_ptr<Discovery::FWUnit> unit) const;

    uint64_t GetUnitGUID(std::shared_ptr<Discovery::FWUnit> unit) const;

    void RebuildNodeIDMap();
    void OnPreparedUnit(std::shared_ptr<Discovery::FWUnit> unit);

    void ScheduleRescan(uint64_t guid, const std::shared_ptr<AVCUnit>& avcUnit);
    [[nodiscard]] bool IsRescanCurrent(const Discovery::DeviceRouteToken& route,
                                       uint64_t operationSerial) const noexcept;

    IOService* driver_{nullptr};
    Discovery::DeviceRegistry& deviceRegistry_;
    Discovery::IDeviceManager& deviceManager_;
    Protocols::Ports::FireWireBusOps& busOps_;
    Protocols::Ports::FireWireBusInfo& busInfo_;
    Scheduling::ITimerScheduler& timerScheduler_;
    std::shared_ptr<DiscoveryOwner> owner_;

    IOLock* lock_{nullptr};

    std::unordered_map<uint64_t, std::shared_ptr<AVCUnit>> units_;

    std::unordered_map<uint16_t, std::shared_ptr<FCPTransport>> fcpTransportsByNodeID_;
    std::unordered_map<uint64_t, uint8_t> rescanAttempts_;
    std::unordered_map<uint64_t, Scheduling::TimerToken> rescanTimersByGuid_;
    std::unordered_map<uint64_t, uint64_t> activeRescanSerialByGuid_;
    uint64_t nextRescanOperationSerial_{0};


    std::atomic<bool> shuttingDown_{false};

    os_log_t log_{OS_LOG_DEFAULT};
};

} // namespace ASFW::Protocols::AVC
