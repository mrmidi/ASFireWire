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
#include "../../Discovery/FWUnit.hpp"
#include "../../Discovery/FWDevice.hpp"
#include "../../Audio/Core/IAVCAudioConfigListener.hpp"
#include "../../Audio/Protocols/Oxford/Apogee/ApogeeTypes.hpp"
#include "../../Scheduling/ITimerScheduler.hpp"

// Forward declarations
namespace ASFW::Discovery { struct DeviceRecord; }
namespace ASFW::Audio::Model { struct ASFWAudioDevice; }
namespace ASFW::Audio::Oxford::Apogee { class ApogeeDuetProtocol; }
namespace ASFW::Protocols::AVC::Music { class MusicSubunit; }
namespace ASFW::Audio::BeBoB { struct DeviceModel; }

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
                 Discovery::IDeviceManager& deviceManager,
                 Protocols::Ports::FireWireBusOps& busOps,
                 Protocols::Ports::FireWireBusInfo& busInfo,
                 Scheduling::ITimerScheduler& timerScheduler,
                 ASFW::Audio::IAVCAudioConfigListener* audioConfigListener);

    ~AVCDiscovery() override;

    AVCDiscovery(const AVCDiscovery&) = delete;
    AVCDiscovery& operator=(const AVCDiscovery&) = delete;

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
    struct DuetPrefetchState {
        std::optional<::ASFW::Audio::Oxford::Apogee::InputParams> inputParams;
        std::optional<::ASFW::Audio::Oxford::Apogee::MixerParams> mixerParams;
        std::optional<::ASFW::Audio::Oxford::Apogee::OutputParams> outputParams;
        std::optional<::ASFW::Audio::Oxford::Apogee::DisplayParams> displayParams;
        std::optional<uint32_t> firmwareId;
        std::optional<uint32_t> hardwareId;
        bool timedOut{false};
    };

    bool IsAVCUnit(std::shared_ptr<Discovery::FWUnit> unit) const;
    bool IsApogeeDuet(const Discovery::FWDevice& device) const noexcept;

    uint64_t GetUnitGUID(std::shared_ptr<Discovery::FWUnit> unit) const;

    void RebuildNodeIDMap();

    void HandleInitializedUnit(uint64_t guid, const std::shared_ptr<AVCUnit>& avcUnit);
    void PublishBeBoBAudioConfig(uint64_t guid,
                                  uint32_t vendorId,
                                  uint32_t modelId,
                                  const std::string& deviceName,
                                  const ::ASFW::Audio::BeBoB::DeviceModel& inventory);
    [[nodiscard]] Music::MusicSubunit* FindAudioMusicSubunit(const AVCUnit& avcUnit) const;
    void PopulateMusicSubunitCapabilities(uint64_t guid,
                                          const Discovery::FWDevice& device,
                                          Music::MusicSubunit& musicSubunit) const;
    void UpdateCurrentSampleRate(Music::MusicSubunit& musicSubunit) const;
    [[nodiscard]] ::ASFW::Audio::Model::ASFWAudioDevice BuildAudioDeviceConfig(uint64_t guid,
                                                                       const Discovery::FWDevice& device,
                                                                       const Music::MusicSubunit& musicSubunit) const;
    void PublishReadyAudioConfig(uint64_t guid, const ::ASFW::Audio::Model::ASFWAudioDevice& config);
    void PrefetchDuetStateAndCreateNub(uint64_t guid,
                                       const std::shared_ptr<AVCUnit>& avcUnit,
                                       const ::ASFW::Audio::Model::ASFWAudioDevice& config);
    void ContinueDuetPrefetchMixer(uint64_t guid,
                                   const std::shared_ptr<::ASFW::Audio::Oxford::Apogee::ApogeeDuetProtocol>& protocol,
                                   const std::shared_ptr<DuetPrefetchState>& state,
                                   const std::shared_ptr<std::atomic<bool>>& completed,
                                   const std::shared_ptr<std::function<void(const char*)>>& finish);
    void ContinueDuetPrefetchOutput(uint64_t guid,
                                    const std::shared_ptr<::ASFW::Audio::Oxford::Apogee::ApogeeDuetProtocol>& protocol,
                                    const std::shared_ptr<DuetPrefetchState>& state,
                                    const std::shared_ptr<std::atomic<bool>>& completed,
                                    const std::shared_ptr<std::function<void(const char*)>>& finish);
    void ContinueDuetPrefetchDisplay(uint64_t guid,
                                     const std::shared_ptr<::ASFW::Audio::Oxford::Apogee::ApogeeDuetProtocol>& protocol,
                                     const std::shared_ptr<DuetPrefetchState>& state,
                                     const std::shared_ptr<std::atomic<bool>>& completed,
                                     const std::shared_ptr<std::function<void(const char*)>>& finish);
    void ContinueDuetPrefetchFirmware(uint64_t guid,
                                      const std::shared_ptr<::ASFW::Audio::Oxford::Apogee::ApogeeDuetProtocol>& protocol,
                                      const std::shared_ptr<DuetPrefetchState>& state,
                                      const std::shared_ptr<std::atomic<bool>>& completed,
                                      const std::shared_ptr<std::function<void(const char*)>>& finish);
    void ContinueDuetPrefetchHardware(uint64_t guid,
                                      const std::shared_ptr<::ASFW::Audio::Oxford::Apogee::ApogeeDuetProtocol>& protocol,
                                      const std::shared_ptr<DuetPrefetchState>& state,
                                      const std::shared_ptr<std::atomic<bool>>& completed,
                                      const std::shared_ptr<std::function<void(const char*)>>& finish);
    void ScheduleRescan(uint64_t guid, const std::shared_ptr<AVCUnit>& avcUnit);

    IOService* driver_{nullptr};
    Discovery::IDeviceManager& deviceManager_;
    Protocols::Ports::FireWireBusOps& busOps_;
    Protocols::Ports::FireWireBusInfo& busInfo_;
    Scheduling::ITimerScheduler& timerScheduler_;
    ASFW::Audio::IAVCAudioConfigListener* audioConfigListener_{nullptr};

    IOLock* lock_{nullptr};

    std::unordered_map<uint64_t, std::shared_ptr<AVCUnit>> units_;

    std::unordered_map<uint16_t, std::shared_ptr<FCPTransport>> fcpTransportsByNodeID_;
    std::unordered_map<uint64_t, uint8_t> rescanAttempts_;
    std::unordered_map<uint64_t, DuetPrefetchState> duetPrefetchByGuid_;

    OSSharedPtr<IODispatchQueue> rescanQueue_;

    std::atomic<bool> shuttingDown_{false};

    os_log_t log_{OS_LOG_DEFAULT};
};

} // namespace ASFW::Protocols::AVC
