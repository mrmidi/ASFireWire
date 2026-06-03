// IsochService.cpp
// ASFW - Isochronous Service (orchestrator for IT/IR contexts)

#include "IsochService.hpp"
#include "../Audio/Core/AudioNubPublisher.hpp"
#include "../Audio/DriverKit/Runtime/DirectAudioBindingSource.hpp"
#include "../Logging/Logging.hpp"
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <DriverKit/IOMemoryMap.h>

namespace ASFW::Driver {

using namespace ASFW::Isoch;

kern_return_t IsochService::StartReceive(uint8_t channel,
                                         HardwareInterface& hardware,
                                         ASFW::Audio::Runtime::IDirectAudioBindingSource* bindingSource,
                                         ASFW::Encoding::AudioWireFormat wireFormat,
                                         uint32_t am824Slots) {
    if (!isochReceiveContext_) {
        isochReceiveContext_ = IsochReceiveContext::Create(&hardware, 0);
        if (!isochReceiveContext_) {
            ASFW_LOG(Isoch, "IsochService: Failed to create IR context");
            return kIOReturnNoMemory;
        }
        isochReceiveContext_->SetExternalSyncBridge(&externalSyncBridge_);
        RefreshReceiveTimingLossCallback();
    }

    isochReceiveContext_->SetDirectAudioBindingSource(bindingSource);

    const kern_return_t kr = isochReceiveContext_->Configure(channel, 0, wireFormat, am824Slots);
    if (kr != kIOReturnSuccess) {
        ASFW_LOG(Isoch, "IsochService: IR Configure failed: 0x%08x", kr);
        return kr;
    }

    ASFW_LOG(Isoch, "IsochService: Starting IR on channel %u (Direct-Only)", channel);
    return isochReceiveContext_->Start();
}

kern_return_t IsochService::StopReceive() {
    if (isochReceiveContext_) {
        isochReceiveContext_->Stop();
        isochReceiveContext_->SetDirectAudioBindingSource(nullptr);
    }
    return kIOReturnSuccess;
}

kern_return_t IsochService::StartTransmit(uint8_t channel,
                                          HardwareInterface& hardware,
                                          uint8_t sid,
                                          uint32_t streamModeRaw,
                                          uint32_t pcmChannels,
                                          uint32_t am824Slots,
                                          ASFW::Encoding::AudioWireFormat wireFormat,
                                          ASFW::Audio::Runtime::IDirectAudioBindingSource* bindingSource) {
    if (!isochTransmitContext_) {
        isochTransmitContext_ = IsochTransmitContext::Create(&hardware, nullptr);
        if (!isochTransmitContext_) {
            ASFW_LOG(Isoch, "IsochService: Failed to create IT context");
            return kIOReturnNoMemory;
        }
        isochTransmitContext_->SetExternalSyncBridge(&externalSyncBridge_);
        RefreshTransmitRecoveryCallback();
    }

    isochTransmitContext_->SetDirectAudioBindingSource(bindingSource);

    const kern_return_t kr = isochTransmitContext_->Configure(
        channel, sid, streamModeRaw, pcmChannels, am824Slots, wireFormat);
    if (kr != kIOReturnSuccess) {
        ASFW_LOG(Isoch, "IsochService: IT Configure failed: 0x%08x", kr);
        return kr;
    }

    ASFW_LOG(Isoch, "IsochService: Starting IT on channel %u (Direct-Only)", channel);
    return isochTransmitContext_->Start();
}

kern_return_t IsochService::StopTransmit() {
    if (isochTransmitContext_) {
        isochTransmitContext_->Stop();
        isochTransmitContext_->SetDirectAudioBindingSource(nullptr);
    }
    return kIOReturnSuccess;
}

kern_return_t IsochService::BeginSplitDuplex(uint64_t guid) {
    const kern_return_t kr = ClaimDuplexGuid(guid);
    if (kr != kIOReturnSuccess) return kr;
    
    reserved_.Reset();
    return kIOReturnSuccess;
}

kern_return_t IsochService::ReservePlaybackResources(uint64_t guid,
                                                     IRM::IRMClient& irmClient,
                                                     uint8_t channel,
                                                     uint32_t bandwidthUnits) {
    if (activeGuid_ != guid) return kIOReturnNotPrivileged;
    
    reserved_.playbackActive = true;
    reserved_.playbackChannel = channel;
    reserved_.playbackBandwidthUnits = bandwidthUnits;
    return kIOReturnSuccess;
}

kern_return_t IsochService::ReserveCaptureResources(uint64_t guid,
                                                    IRM::IRMClient& irmClient,
                                                    uint8_t channel,
                                                    uint32_t bandwidthUnits) {
    if (activeGuid_ != guid) return kIOReturnNotPrivileged;
    
    reserved_.captureActive = true;
    reserved_.captureChannel = channel;
    reserved_.captureBandwidthUnits = bandwidthUnits;
    return kIOReturnSuccess;
}

kern_return_t IsochService::StartDuplex(const IsochDuplexStartParams& params,
                                        HardwareInterface& hardware) {
    if (activeGuid_ != params.guid) return kIOReturnNotPrivileged;

    ASFW_LOG(Isoch, "IsochService: Starting Direct-Only Duplex guid=0x%llx", params.guid);

    if (params.hostInputPcmChannels > 0) {
        const kern_return_t kr = StartReceive(params.irChannel,
                                               hardware,
                                               params.directAudioBindingSource,
                                               params.deviceToHostWireFormat,
                                               params.deviceToHostAm824Slots);
        if (kr != kIOReturnSuccess) {
            StopAll();
            return kr;
        }
    }

    if (params.hostOutputPcmChannels > 0) {
        const kern_return_t kr = StartTransmit(params.itChannel,
                                                hardware,
                                                params.sid,
                                                std::to_underlying(params.streamMode),
                                                params.hostOutputPcmChannels,
                                                params.hostToDeviceAm824Slots,
                                                params.hostToDeviceWireFormat,
                                                params.directAudioBindingSource);
        if (kr != kIOReturnSuccess) {
            StopAll();
            return kr;
        }
    }

    return kIOReturnSuccess;
}

kern_return_t IsochService::StopDuplex(uint64_t guid, IRM::IRMClient* irmClient) {
    if (activeGuid_ != guid) return kIOReturnNotPrivileged;
    
    StopReceive();
    StopTransmit();
    
    reserved_.Reset();
    activeGuid_ = 0;
    return kIOReturnSuccess;
}

void IsochService::StopAll() {
    StopReceive();
    StopTransmit();
    reserved_.Reset();
    activeGuid_ = 0;
}

void IsochService::SetTimingLossCallback(TimingLossCallback callback) noexcept {
    timingLossCallback_ = std::move(callback);
}

void IsochService::SetTxRecoveryCallback(TxRecoveryCallback callback) noexcept {
    txRecoveryCallback_ = std::move(callback);
}

kern_return_t IsochService::ClaimDuplexGuid(uint64_t guid) {
    if (activeGuid_ != 0 && activeGuid_ != guid) {
        ASFW_LOG(Isoch, "IsochService: GUID conflict 0x%llx (active: 0x%llx)",
                 guid, activeGuid_);
        return kIOReturnBusy;
    }
    activeGuid_ = guid;
    return kIOReturnSuccess;
}

void IsochService::RefreshReceiveTimingLossCallback() noexcept {
    if (isochReceiveContext_) {
        isochReceiveContext_->SetTimingLossCallback([this]() {
            OnReceiveTimingLossDetected();
        });
    }
}

void IsochService::RefreshTransmitRecoveryCallback() noexcept {
    if (isochTransmitContext_) {
        isochTransmitContext_->SetRecoveryCallback([this](uint32_t reasonBits) {
            return OnTransmitRecoveryRequested(reasonBits);
        });
    }
}

void IsochService::OnReceiveTimingLossDetected() noexcept {
    if (timingLossCallback_ && activeGuid_ != 0) {
        timingLossCallback_(activeGuid_);
    }
}

bool IsochService::OnTransmitRecoveryRequested(uint32_t reasonBits) noexcept {
    if (txRecoveryCallback_ && activeGuid_ != 0) {
        return txRecoveryCallback_(activeGuid_, reasonBits);
    }
    return false;
}

} // namespace ASFW::Driver
