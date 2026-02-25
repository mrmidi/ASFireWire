// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2026 ASFireWire Project

#include "DiceAudioBackend.hpp"

#include "../../Logging/Logging.hpp"
#include "../../Protocols/Audio/DeviceProtocolFactory.hpp"

#include <DriverKit/IOLib.h>
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <DriverKit/OSSharedPtr.h>
#include <net.mrmidi.ASFW.ASFWDriver/ASFWAudioNub.h>
#include <string>

namespace ASFW::Audio {

namespace {

constexpr uint8_t kDefaultIrChannel = 0;
constexpr uint8_t kDefaultItChannel = 1;

inline uint8_t ReadLocalSid(Driver::HardwareInterface& hw) noexcept {
    // OHCI NodeID register: low 6 bits are node number.
    return static_cast<uint8_t>(hw.ReadNodeID() & 0x3Fu);
}

} // namespace

DiceAudioBackend::DiceAudioBackend(AudioNubPublisher& publisher,
                                   Discovery::DeviceRegistry& registry,
                                   Driver::IsochService& isoch,
                                   Driver::HardwareInterface& hardware) noexcept
    : publisher_(publisher)
    , registry_(registry)
    , isoch_(isoch)
    , hardware_(hardware) {
    lock_ = IOLockAlloc();
    if (!lock_) {
        ASFW_LOG_ERROR(Audio, "DiceAudioBackend: Failed to allocate lock");
    }

    IODispatchQueue* queue = nullptr;
    const kern_return_t kr = IODispatchQueue::Create("com.asfw.audio.dice", 0, 0, &queue);
    if (kr == kIOReturnSuccess && queue) {
        workQueue_ = OSSharedPtr(queue, OSNoRetain);
    } else {
        ASFW_LOG_ERROR(Audio, "DiceAudioBackend: Failed to create work queue (0x%x)", kr);
    }
}

DiceAudioBackend::~DiceAudioBackend() noexcept {
    if (lock_) {
        IOLockFree(lock_);
        lock_ = nullptr;
    }
}

void DiceAudioBackend::OnDeviceRecordUpdated(uint64_t guid) noexcept {
    EnsureNubForGuid(guid);
}

void DiceAudioBackend::OnDeviceRemoved(uint64_t guid) noexcept {
    if (guid == 0) return;

    (void)StopStreaming(guid);
    publisher_.TerminateNub(guid, "DICE-Removed");

    if (lock_) {
        IOLockLock(lock_);
        attemptsByGuid_.erase(guid);
        retryOutstanding_.erase(guid);
        IOLockUnlock(lock_);
    }
}

void DiceAudioBackend::EnsureNubForGuid(uint64_t guid) noexcept {
    if (guid == 0) return;

    const auto* record = registry_.FindByGuid(guid);
    if (!record) return;

    const auto integration = DeviceProtocolFactory::LookupIntegrationMode(record->vendorId, record->modelId);
    if (integration != DeviceIntegrationMode::kHardcodedNub) {
        return;
    }

    if (!record->protocol) {
        return;
    }

    AudioStreamRuntimeCaps caps{};
    const bool ready = record->protocol->GetRuntimeAudioStreamCaps(caps);

    if (!ready || caps.sampleRateHz == 0 || caps.hostInputPcmChannels == 0 || caps.hostOutputPcmChannels == 0) {
        bool shouldRetry = false;
        uint8_t attempt = 0;
        bool outstanding = false;

        if (lock_) {
            IOLockLock(lock_);
            attempt = attemptsByGuid_[guid];
            outstanding = (retryOutstanding_.find(guid) != retryOutstanding_.end());
            if (attempt < kCapsRetryMaxAttempts && !outstanding) {
                attemptsByGuid_[guid] = static_cast<uint8_t>(attempt + 1u);
                retryOutstanding_.insert(guid);
                shouldRetry = true;
                attempt = static_cast<uint8_t>(attempt + 1u);
            }
            IOLockUnlock(lock_);
        }

        if (outstanding) {
            return;
        }

        if (shouldRetry && workQueue_) {
            ASFW_LOG(Audio,
                     "DiceAudioBackend: runtime caps not ready for GUID=%llx; retry %u/%u in %u ms",
                     guid,
                     attempt,
                     kCapsRetryMaxAttempts,
                     kCapsRetryDelayMs);
            workQueue_->DispatchAsync(^{
                IOSleep(kCapsRetryDelayMs);
                if (lock_) {
                    IOLockLock(lock_);
                    retryOutstanding_.erase(guid);
                    IOLockUnlock(lock_);
                }
                EnsureNubForGuid(guid);
            });
            return;
        }

        // Fallback profile for known Focusrite Saffire Pro 24 DSP only.
        if (record->vendorId == DeviceProtocolFactory::kFocusriteVendorId &&
            record->modelId == DeviceProtocolFactory::kSPro24DspModelId) {
            caps.sampleRateHz = 48000;
            caps.hostInputPcmChannels = 16;
            caps.hostOutputPcmChannels = 8;
            caps.deviceToHostAm824Slots = 17;
            caps.hostToDeviceAm824Slots = 9;
            ASFW_LOG_WARNING(Audio,
                             "DiceAudioBackend: runtime caps still not ready for GUID=%llx; using SPro24DSP fallback 16/8",
                             guid);
        } else {
            ASFW_LOG_ERROR(Audio,
                           "DiceAudioBackend: runtime caps not ready for GUID=%llx; refusing to publish a lying nub",
                           guid);
            return;
        }
    }

    Model::ASFWAudioDevice dev{};
    dev.guid = record->guid;
    dev.vendorId = record->vendorId;
    dev.modelId = record->modelId;
    dev.deviceName = !record->vendorName.empty() && !record->modelName.empty()
        ? (record->vendorName + " " + record->modelName)
        : std::string(record->protocol ? record->protocol->GetName() : "DICE Audio");
    dev.inputPlugName = "Input";
    dev.outputPlugName = "Output";

    dev.currentSampleRate = caps.sampleRateHz ? caps.sampleRateHz : 48000;
    dev.sampleRates = {dev.currentSampleRate};

    dev.inputChannelCount = caps.hostInputPcmChannels;
    dev.outputChannelCount = caps.hostOutputPcmChannels;
    dev.channelCount = (dev.inputChannelCount > dev.outputChannelCount)
        ? dev.inputChannelCount
        : dev.outputChannelCount;

    // DICE family policy: 48k uses blocking cadence (NDDD).
    dev.streamMode = Model::StreamMode::kBlocking;

    (void)publisher_.EnsureNub(guid, dev, "DICE");

    if (lock_) {
        IOLockLock(lock_);
        attemptsByGuid_.erase(guid);
        retryOutstanding_.erase(guid);
        IOLockUnlock(lock_);
    }
}

IOReturn DiceAudioBackend::StartStreaming(uint64_t guid) noexcept {
    if (guid == 0) return kIOReturnBadArgument;

    auto* record = registry_.FindByGuid(guid);
    if (!record || !record->protocol) {
        return kIOReturnNotReady;
    }

    AudioStreamRuntimeCaps caps{};
    if (!record->protocol->GetRuntimeAudioStreamCaps(caps)) {
        return kIOReturnNotReady;
    }

    auto* nub = publisher_.GetNub(guid);
    if (!nub) {
        EnsureNubForGuid(guid);
        nub = publisher_.GetNub(guid);
        if (!nub) return kIOReturnNotReady;
    }

    // Ensure queues exist before wiring to isoch contexts.
    nub->EnsureRxQueueCreated();
    OSSharedPtr<IOBufferMemoryDescriptor> rxMem{};
    uint64_t rxBytes = 0;
    const kern_return_t rxCopy = nub->CopyRxQueueMemory(rxMem.attach(), &rxBytes);
    if (rxCopy != kIOReturnSuccess || !rxMem || rxBytes == 0) {
        return (rxCopy == kIOReturnSuccess) ? kIOReturnNoMemory : rxCopy;
    }

    OSSharedPtr<IOBufferMemoryDescriptor> txMem{};
    uint64_t txBytes = 0;
    const kern_return_t txCopy = nub->CopyTransmitQueueMemory(txMem.attach(), &txBytes);
    if (txCopy != kIOReturnSuccess || !txMem || txBytes == 0) {
        return (txCopy == kIOReturnSuccess) ? kIOReturnNoMemory : txCopy;
    }

    const IOReturn cfg = record->protocol->StartDuplex48k();
    if (cfg != kIOReturnSuccess && cfg != kIOReturnUnsupported) {
        ASFW_LOG_ERROR(Audio, "DiceAudioBackend: StartDuplex48k failed GUID=%llx status=0x%x", guid, cfg);
        return cfg;
    }

    Driver::IsochDuplexStartParams params{};
    params.guid = guid;
    params.irChannel = kDefaultIrChannel;
    params.itChannel = kDefaultItChannel;
    params.sid = ReadLocalSid(hardware_);
    params.sampleRateHz = caps.sampleRateHz;
    params.hostInputPcmChannels = caps.hostInputPcmChannels;
    params.hostOutputPcmChannels = caps.hostOutputPcmChannels;
    params.deviceToHostAm824Slots = caps.deviceToHostAm824Slots;
    params.hostToDeviceAm824Slots = caps.hostToDeviceAm824Slots;
    params.streamMode = Model::StreamMode::kBlocking;

    params.rxQueueMemory = rxMem.detach();
    params.rxQueueBytes = rxBytes;
    params.txQueueMemory = txMem.detach();
    params.txQueueBytes = txBytes;

    // DICE playback: no zero-copy for now (explicitly disabled by policy).
    params.zeroCopyBase = nullptr;
    params.zeroCopyBytes = 0;
    params.zeroCopyFrames = 0;

    const kern_return_t kr = isoch_.StartDuplex(params, hardware_);
    if (kr != kIOReturnSuccess) {
        ASFW_LOG_ERROR(Audio, "DiceAudioBackend: StartDuplex failed GUID=%llx kr=0x%x", guid, kr);
    }
    return kr;
}

IOReturn DiceAudioBackend::StopStreaming(uint64_t guid) noexcept {
    if (guid == 0) return kIOReturnBadArgument;
    return isoch_.StopDuplex(guid);
}

} // namespace ASFW::Audio
