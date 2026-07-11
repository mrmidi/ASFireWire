// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// IsochDuplexHostTransport.hpp - Host-side isoch orchestration seam

#pragma once

#include "../../../Common/WireFormat.hpp"
#include "../../../Hardware/HardwareInterface.hpp"
#include "../../../Isoch/IsochService.hpp"
#include "DuplexIRMReservations.hpp"

#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <DriverKit/IOReturn.h>
#include <DriverKit/OSSharedPtr.h>


namespace ASFW::IRM {
class IRMClient;
}

class ASFWAudioNub;

namespace ASFW::Audio {

class IIsochDuplexHostTransport {
  public:
    virtual ~IIsochDuplexHostTransport() = default;

    [[nodiscard]] virtual kern_return_t BeginSplitDuplex(uint64_t guid) noexcept = 0;
    [[nodiscard]] virtual kern_return_t
    ReservePlaybackResources(uint64_t guid, ::ASFW::IRM::IRMClient& irmClient, uint8_t channel,
                             uint32_t bandwidthUnits) noexcept = 0;
    [[nodiscard]] virtual kern_return_t
    ReserveCaptureResources(uint64_t guid, ::ASFW::IRM::IRMClient& irmClient, uint8_t channel,
                            uint32_t bandwidthUnits) noexcept = 0;
    [[nodiscard]] virtual kern_return_t
    PrepareReceive(uint8_t channel, Driver::HardwareInterface& hardware,
                   ASFW::Audio::Runtime::IDirectAudioBindingSource* bindingSource,
                   Encoding::AudioWireFormat wireFormat = Encoding::AudioWireFormat::kAM824,
                   uint32_t am824Slots = 0, uint32_t streamChannels = 0) noexcept = 0;
    [[nodiscard]] virtual kern_return_t PrepareTransmit(uint8_t channel,
                                                        Driver::HardwareInterface& hardware,
                                                        uint8_t sourceId) noexcept = 0;
    // Secondary streams (streamIndex >= 1) for multi-stream DICE devices; the
    // master stream uses PrepareReceive/PrepareTransmit above.
    [[nodiscard]] virtual kern_return_t
    PrepareReceiveStream(uint32_t streamIndex, uint8_t channel, Driver::HardwareInterface& hardware,
                         ASFW::Audio::Runtime::IDirectAudioBindingSource* bindingSource,
                         uint32_t channelOffset, uint32_t streamChannels,
                         Encoding::AudioWireFormat wireFormat = Encoding::AudioWireFormat::kAM824,
                         uint32_t am824Slots = 0) noexcept = 0;
    [[nodiscard]] virtual kern_return_t PrepareTransmitStream(uint32_t streamIndex, uint8_t channel,
                                                              Driver::HardwareInterface& hardware,
                                                              uint8_t sourceId) noexcept = 0;
    [[nodiscard]] virtual kern_return_t StartPreparedReceive() noexcept = 0;
    [[nodiscard]] virtual kern_return_t StartPreparedTransmit() noexcept = 0;
    [[nodiscard]] virtual kern_return_t StopPreparedReceive() noexcept {
        return kIOReturnUnsupported;
    }
    [[nodiscard]] virtual kern_return_t StopPreparedTransmit() noexcept {
        return kIOReturnUnsupported;
    }
    [[nodiscard]] virtual kern_return_t StopAll() noexcept = 0;
};

class IsochDuplexHostTransport final : public IIsochDuplexHostTransport {
  public:
    explicit IsochDuplexHostTransport(Driver::IsochService& isoch) noexcept : isoch_(isoch) {}

    void SetTimingLossCallback(Driver::IsochService::TimingLossCallback callback) noexcept;

    [[nodiscard]] kern_return_t BeginSplitDuplex(uint64_t guid) noexcept override;
    [[nodiscard]] kern_return_t ReservePlaybackResources(uint64_t guid,
                                                         ::ASFW::IRM::IRMClient& irmClient,
                                                         uint8_t channel,
                                                         uint32_t bandwidthUnits) noexcept override;
    [[nodiscard]] kern_return_t ReserveCaptureResources(uint64_t guid,
                                                        ::ASFW::IRM::IRMClient& irmClient,
                                                        uint8_t channel,
                                                        uint32_t bandwidthUnits) noexcept override;
    [[nodiscard]] kern_return_t
    PrepareReceive(uint8_t channel, Driver::HardwareInterface& hardware,
                   ASFW::Audio::Runtime::IDirectAudioBindingSource* bindingSource,
                   Encoding::AudioWireFormat wireFormat = Encoding::AudioWireFormat::kAM824,
                   uint32_t am824Slots = 0, uint32_t streamChannels = 0) noexcept override;
    [[nodiscard]] kern_return_t PrepareTransmit(uint8_t channel,
                                                Driver::HardwareInterface& hardware,
                                                uint8_t sourceId) noexcept override;
    [[nodiscard]] kern_return_t
    PrepareReceiveStream(uint32_t streamIndex, uint8_t channel, Driver::HardwareInterface& hardware,
                         ASFW::Audio::Runtime::IDirectAudioBindingSource* bindingSource,
                         uint32_t channelOffset, uint32_t streamChannels,
                         Encoding::AudioWireFormat wireFormat = Encoding::AudioWireFormat::kAM824,
                         uint32_t am824Slots = 0) noexcept override;
    [[nodiscard]] kern_return_t PrepareTransmitStream(uint32_t streamIndex, uint8_t channel,
                                                      Driver::HardwareInterface& hardware,
                                                      uint8_t sourceId) noexcept override;
    [[nodiscard]] kern_return_t StartPreparedReceive() noexcept override;
    [[nodiscard]] kern_return_t StartPreparedTransmit() noexcept override;
    [[nodiscard]] kern_return_t StopPreparedReceive() noexcept override;
    [[nodiscard]] kern_return_t StopPreparedTransmit() noexcept override;
    [[nodiscard]] kern_return_t StopAll() noexcept override;

  private:
    Driver::IsochService& isoch_;
    Backends::DuplexIRMReservationPair reservations_{};
};

} // namespace ASFW::Audio
