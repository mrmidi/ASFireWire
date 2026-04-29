// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2026 ASFireWire Project

#include "DiceHostTransport.hpp"

#include "../../Common/DriverKitOwnership.hpp"

#include "ASFWAudioNub.h"
#include <utility>

namespace ASFW::Audio {

void DiceIsochHostTransport::SetTimingLossCallback(
    Driver::IsochService::TimingLossCallback callback) noexcept {
    isoch_.SetTimingLossCallback(std::move(callback));
}

void DiceIsochHostTransport::SetTxRecoveryCallback(
    Driver::IsochService::TxRecoveryCallback callback) noexcept {
    isoch_.SetTxRecoveryCallback(std::move(callback));
}

kern_return_t DiceNubQueueMemoryProvider::CopyRxQueueMemory(
    OSSharedPtr<IOBufferMemoryDescriptor>& outMem,
    uint64_t& outBytes) noexcept {
    nub_.EnsureRxQueueCreated();
    IOBufferMemoryDescriptor* raw = nullptr;
    outBytes = 0;
    const kern_return_t status = nub_.CopyRxQueueMemory(&raw, &outBytes);
    outMem = Common::AdoptRetained(raw);
    return status;
}

kern_return_t DiceNubQueueMemoryProvider::CopyTransmitQueueMemory(
    OSSharedPtr<IOBufferMemoryDescriptor>& outMem,
    uint64_t& outBytes) noexcept {
    IOBufferMemoryDescriptor* raw = nullptr;
    outBytes = 0;
    const kern_return_t status = nub_.CopyTransmitQueueMemory(&raw, &outBytes);
    outMem = Common::AdoptRetained(raw);
    return status;
}

kern_return_t DiceIsochHostTransport::BeginSplitDuplex(uint64_t guid) noexcept {
    return isoch_.BeginSplitDuplex(guid);
}

kern_return_t DiceIsochHostTransport::ReservePlaybackResources(uint64_t guid,
                                                               ::ASFW::IRM::IRMClient& irmClient,
                                                               uint8_t channel,
                                                               uint32_t bandwidthUnits) noexcept {
    return isoch_.ReservePlaybackResources(guid, irmClient, channel, bandwidthUnits);
}

kern_return_t DiceIsochHostTransport::ReserveCaptureResources(uint64_t guid,
                                                              ::ASFW::IRM::IRMClient& irmClient,
                                                              uint8_t channel,
                                                              uint32_t bandwidthUnits) noexcept {
    return isoch_.ReserveCaptureResources(guid, irmClient, channel, bandwidthUnits);
}

kern_return_t DiceIsochHostTransport::StartReceive(
    uint8_t channel,
    Driver::HardwareInterface& hardware,
    const OSSharedPtr<IOBufferMemoryDescriptor>& rxMem,
    uint64_t rxBytes) noexcept {
    return isoch_.StartReceive(channel, hardware, rxMem, rxBytes);
}

kern_return_t DiceIsochHostTransport::StartTransmit(
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
    uint32_t zeroCopyFrames) noexcept {
    return isoch_.StartTransmit(channel,
                                hardware,
                                sourceId,
                                streamModeRaw,
                                pcmChannels,
                                dataBlockSize,
                                wireFormat,
                                txMem,
                                txBytes,
                                zeroCopyBase,
                                zeroCopyBytes,
                                zeroCopyFrames);
}

kern_return_t DiceIsochHostTransport::StopDuplex(
    uint64_t guid,
    ::ASFW::IRM::IRMClient* irmClient) noexcept {
    return isoch_.StopDuplex(guid, irmClient);
}

} // namespace ASFW::Audio
