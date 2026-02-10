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
#include <memory>
#include <unordered_map>
#include "IAVCDiscovery.hpp"
#include "AVCUnit.hpp"
#include "../../Discovery/IDeviceManager.hpp"
#include "../../Discovery/FWUnit.hpp"
#include "../../Discovery/FWDevice.hpp"
#include "../../Async/AsyncSubsystem.hpp"

// Forward declarations
class ASFWAudioNub;
namespace ASFW::Discovery { struct DeviceRecord; }
namespace ASFW::Audio::Model { struct ASFWAudioDevice; }

namespace ASFW::Protocols::AVC {

//==============================================================================
// AV/C Discovery
//==============================================================================

class AVCDiscovery : public Discovery::IUnitObserver, public IAVCDiscovery {
public:
    AVCDiscovery(IOService* driver,
                 Discovery::IDeviceManager& deviceManager,
                 Async::AsyncSubsystem& asyncSubsystem);

    ~AVCDiscovery() override;

    AVCDiscovery(const AVCDiscovery&) = delete;
    AVCDiscovery& operator=(const AVCDiscovery&) = delete;

    void OnUnitPublished(std::shared_ptr<Discovery::FWUnit> unit) override;
    void OnUnitSuspended(std::shared_ptr<Discovery::FWUnit> unit) override;
    void OnUnitResumed(std::shared_ptr<Discovery::FWUnit> unit) override;
    void OnUnitTerminated(std::shared_ptr<Discovery::FWUnit> unit) override;

    AVCUnit* GetAVCUnit(uint64_t guid);

    AVCUnit* GetAVCUnit(std::shared_ptr<Discovery::FWUnit> unit);

    std::vector<AVCUnit*> GetAllAVCUnits() override;

    void ReScanAllUnits() override;

    FCPTransport* GetFCPTransportForNodeID(uint16_t nodeID);

    void OnBusReset(uint32_t newGeneration);

    void SetTransmitRingBufferOnNubs(void* ringBuffer);

    ASFWAudioNub* GetFirstAudioNub();

    // Create audio nub from hardcoded profile for known non-AV/C bring-up.
    void EnsureHardcodedAudioNubForDevice(const Discovery::DeviceRecord& deviceRecord);

private:
    bool IsAVCUnit(std::shared_ptr<Discovery::FWUnit> unit) const;

    uint64_t GetUnitGUID(std::shared_ptr<Discovery::FWUnit> unit) const;

    void RebuildNodeIDMap();

    void HandleInitializedUnit(uint64_t guid, const std::shared_ptr<AVCUnit>& avcUnit);
    bool CreateAudioNubFromModel(uint64_t guid,
                                 const Audio::Model::ASFWAudioDevice& config,
                                 const char* sourceTag);
    void ScheduleRescan(uint64_t guid, const std::shared_ptr<AVCUnit>& avcUnit);

    IOService* driver_{nullptr};
    Discovery::IDeviceManager& deviceManager_;
    Async::AsyncSubsystem& asyncSubsystem_;

    IOLock* lock_{nullptr};

    std::unordered_map<uint64_t, std::shared_ptr<AVCUnit>> units_;

    std::unordered_map<uint16_t, FCPTransport*> fcpTransportsByNodeID_;

    std::unordered_map<uint64_t, ASFWAudioNub*> audioNubs_;
    std::unordered_map<uint64_t, uint8_t> rescanAttempts_;

    OSSharedPtr<IODispatchQueue> rescanQueue_;

    os_log_t log_{OS_LOG_DEFAULT};
};

} // namespace ASFW::Protocols::AVC
