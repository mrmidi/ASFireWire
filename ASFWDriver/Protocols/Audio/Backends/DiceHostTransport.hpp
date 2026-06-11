// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2026 ASFireWire Project
//
// DiceHostTransport.hpp - Host-side isoch orchestration seam for DICE restart FSM

#pragma once

#include "../../../Common/WireFormat.hpp"
#include "../../../Hardware/HardwareInterface.hpp"
#include "../../../Isoch/IsochService.hpp"

#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <DriverKit/IOReturn.h>
#include <DriverKit/OSSharedPtr.h>

namespace ASFW::IRM {
class IRMClient;
}

class ASFWAudioNub;

namespace ASFW::Audio {

class IDiceHostTransport {
public:
    virtual ~IDiceHostTransport() = default;

    [[nodiscard]] virtual kern_return_t BeginSplitDuplex(uint64_t guid) noexcept = 0;
    [[nodiscard]] virtual kern_return_t ReservePlaybackResources(uint64_t guid,
                                                                 ::ASFW::IRM::IRMClient& irmClient,
                                                                 uint8_t channel,
                                                                 uint32_t bandwidthUnits) noexcept = 0;
    [[nodiscard]] virtual kern_return_t ReserveCaptureResources(uint64_t guid,
                                                                ::ASFW::IRM::IRMClient& irmClient,
                                                                uint8_t channel,
                                                                uint32_t bandwidthUnits) noexcept = 0;
    [[nodiscard]] virtual kern_return_t StartReceive(
        uint8_t channel,
        Driver::HardwareInterface& hardware,
        ASFW::Audio::Runtime::IDirectAudioBindingSource* bindingSource,
        Encoding::AudioWireFormat wireFormat = Encoding::AudioWireFormat::kAM824,
        uint32_t am824Slots = 0) noexcept = 0;
    [[nodiscard]] virtual kern_return_t StartTransmit(
        uint8_t channel,
        Driver::HardwareInterface& hardware,
        uint8_t sourceId,
        uint32_t streamModeRaw,
        uint32_t pcmChannels,
        uint32_t dataBlockSize,
        Encoding::AudioWireFormat wireFormat,
        ASFW::Audio::Runtime::IDirectAudioBindingSource* bindingSource) noexcept = 0;
    [[nodiscard]] virtual kern_return_t StopDuplex(
        uint64_t guid,
        ::ASFW::IRM::IRMClient* irmClient) noexcept = 0;
};

class DiceIsochHostTransport final : public IDiceHostTransport {
public:
    explicit DiceIsochHostTransport(Driver::IsochService& isoch) noexcept
        : isoch_(isoch) {}

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
    [[nodiscard]] kern_return_t StartReceive(
        uint8_t channel,
        Driver::HardwareInterface& hardware,
        ASFW::Audio::Runtime::IDirectAudioBindingSource* bindingSource,
        Encoding::AudioWireFormat wireFormat = Encoding::AudioWireFormat::kAM824,
        uint32_t am824Slots = 0) noexcept override;
    [[nodiscard]] kern_return_t StartTransmit(
        uint8_t channel,
        Driver::HardwareInterface& hardware,
        uint8_t sourceId,
        uint32_t streamModeRaw,
        uint32_t pcmChannels,
        uint32_t dataBlockSize,
        Encoding::AudioWireFormat wireFormat,
        ASFW::Audio::Runtime::IDirectAudioBindingSource* bindingSource) noexcept override;
    [[nodiscard]] kern_return_t StopDuplex(
        uint64_t guid,
        ::ASFW::IRM::IRMClient* irmClient) noexcept override;

private:
    Driver::IsochService& isoch_;
};

} // namespace ASFW::Audio
