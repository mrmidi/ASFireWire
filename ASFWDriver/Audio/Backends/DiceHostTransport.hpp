// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2026 ASFireWire Project
//
// DiceHostTransport.hpp - Host-side isoch orchestration seam for DICE restart FSM

#pragma once

#include "../../Common/WireFormat.hpp"
#include "../../Hardware/HardwareInterface.hpp"
#include "../../Isoch/IsochService.hpp"

#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <DriverKit/IOReturn.h>
#include <DriverKit/OSSharedPtr.h>

namespace ASFW::IRM {
class IRMClient;
}

class ASFWAudioNub;

namespace ASFW::Audio {

class IDiceQueueMemoryProvider {
public:
    virtual ~IDiceQueueMemoryProvider() = default;

    [[nodiscard]] virtual kern_return_t CopyRxQueueMemory(
        OSSharedPtr<IOBufferMemoryDescriptor>& outMem,
        uint64_t& outBytes) noexcept = 0;
    [[nodiscard]] virtual kern_return_t CopyTransmitQueueMemory(
        OSSharedPtr<IOBufferMemoryDescriptor>& outMem,
        uint64_t& outBytes) noexcept = 0;
};

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
        const OSSharedPtr<IOBufferMemoryDescriptor>& rxMem,
        uint64_t rxBytes) noexcept = 0;
    [[nodiscard]] virtual kern_return_t StartTransmit(
        uint8_t channel,
        Driver::HardwareInterface& hardware,
        uint8_t sourceId,
        uint32_t streamModeRaw,
        uint32_t pcmChannels,
        uint32_t dataBlockSize,
        Encoding::AudioWireFormat wireFormat,
        const OSSharedPtr<IOBufferMemoryDescriptor>& txMem,
        uint64_t txBytes,
        const int32_t* zeroCopyBase,
        uint64_t zeroCopyBytes,
        uint32_t zeroCopyFrames) noexcept = 0;
    [[nodiscard]] virtual kern_return_t StopDuplex(
        uint64_t guid,
        ::ASFW::IRM::IRMClient* irmClient) noexcept = 0;
};

class DiceNubQueueMemoryProvider final : public IDiceQueueMemoryProvider {
public:
    explicit DiceNubQueueMemoryProvider(ASFWAudioNub& nub) noexcept
        : nub_(nub) {}

    [[nodiscard]] kern_return_t CopyRxQueueMemory(
        OSSharedPtr<IOBufferMemoryDescriptor>& outMem,
        uint64_t& outBytes) noexcept override;
    [[nodiscard]] kern_return_t CopyTransmitQueueMemory(
        OSSharedPtr<IOBufferMemoryDescriptor>& outMem,
        uint64_t& outBytes) noexcept override;

private:
    ASFWAudioNub& nub_;
};

class DiceIsochHostTransport final : public IDiceHostTransport {
public:
    explicit DiceIsochHostTransport(Driver::IsochService& isoch) noexcept
        : isoch_(isoch) {}

    void SetTimingLossCallback(Driver::IsochService::TimingLossCallback callback) noexcept;
    void SetTxRecoveryCallback(Driver::IsochService::TxRecoveryCallback callback) noexcept;

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
        const OSSharedPtr<IOBufferMemoryDescriptor>& rxMem,
        uint64_t rxBytes) noexcept override;
    [[nodiscard]] kern_return_t StartTransmit(
        uint8_t channel,
        Driver::HardwareInterface& hardware,
        uint8_t sourceId,
        uint32_t streamModeRaw,
        uint32_t pcmChannels,
        uint32_t dataBlockSize,
        Encoding::AudioWireFormat wireFormat,
        const OSSharedPtr<IOBufferMemoryDescriptor>& txMem,
        uint64_t txBytes,
        const int32_t* zeroCopyBase,
        uint64_t zeroCopyBytes,
        uint32_t zeroCopyFrames) noexcept override;
    [[nodiscard]] kern_return_t StopDuplex(
        uint64_t guid,
        ::ASFW::IRM::IRMClient* irmClient) noexcept override;

private:
    Driver::IsochService& isoch_;
};

} // namespace ASFW::Audio
