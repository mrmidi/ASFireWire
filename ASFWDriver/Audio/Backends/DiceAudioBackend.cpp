// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2026 ASFireWire Project

#include "DiceAudioBackend.hpp"

#include "../../Common/DriverKitOwnership.hpp"
#include "../../Logging/Logging.hpp"
#include "../../Protocols/Audio/DeviceProtocolFactory.hpp"

#include <DriverKit/IOLib.h>
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <DriverKit/OSSharedPtr.h>
#include <net.mrmidi.ASFW.ASFWDriver/ASFWAudioNub.h>
#include <string>

namespace ASFW::Audio {

namespace {

// Match the clean original saffire.kext bring-up on SPro24DSP:
// DICE RX (host->device playback) uses isoch channel 0 and
// DICE TX (device->host capture) uses isoch channel 1.
constexpr uint8_t kDefaultIrChannel = 1;
constexpr uint8_t kDefaultItChannel = 0;

inline uint8_t ReadLocalSid(Driver::HardwareInterface& hw) noexcept {
    // OHCI NodeID register: low 6 bits are node number.
    return static_cast<uint8_t>(hw.ReadNodeID() & 0x3Fu);
}

[[nodiscard]] constexpr Encoding::AudioWireFormat ResolveDicePlaybackWireFormat(
    const Discovery::DeviceRecord& record,
    const AudioStreamRuntimeCaps& caps) noexcept {
    if (record.vendorId == DeviceProtocolFactory::kFocusriteVendorId &&
        record.modelId == DeviceProtocolFactory::kSPro24DspModelId &&
        caps.hostOutputPcmChannels == 8 &&
        caps.hostToDeviceAm824Slots == 9) {
        return Encoding::AudioWireFormat::kRawPcm24In32;
    }
    return Encoding::AudioWireFormat::kAM824;
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
    IOBufferMemoryDescriptor* rxMemRaw = nullptr;
    uint64_t rxBytes = 0;
    const kern_return_t rxCopy = nub->CopyRxQueueMemory(&rxMemRaw, &rxBytes);
    auto rxMem = Common::AdoptRetained(rxMemRaw);
    if (rxCopy != kIOReturnSuccess || !rxMem || rxBytes == 0) {
        return (rxCopy == kIOReturnSuccess) ? kIOReturnNoMemory : rxCopy;
    }

    const AudioDuplexChannels channels{
        .deviceToHostIsoChannel = kDefaultIrChannel,
        .hostToDeviceIsoChannel = kDefaultItChannel,
    };

    const kern_return_t claimStatus = isoch_.BeginSplitDuplex(guid);
    if (claimStatus != kIOReturnSuccess) {
        ASFW_LOG_ERROR(Audio,
                       "DiceAudioBackend: BeginSplitDuplex failed GUID=%llx kr=0x%x",
                       guid,
                       claimStatus);
        return claimStatus;
    }

    const auto rollback = [this, record, guid]() {
        const kern_return_t stopKr = isoch_.StopDuplex(guid);
        if (stopKr != kIOReturnSuccess) {
            ASFW_LOG_ERROR(Audio,
                           "DiceAudioBackend: host duplex rollback failed GUID=%llx kr=0x%x",
                           guid,
                           stopKr);
        }
        const IOReturn stopStatus = record->protocol->StopDuplex();
        if (stopStatus != kIOReturnSuccess && stopStatus != kIOReturnUnsupported) {
            ASFW_LOG_ERROR(Audio,
                           "DiceAudioBackend: StopDuplex rollback failed GUID=%llx status=0x%x",
                           guid,
                           stopStatus);
        }
    };

    const IOReturn cfg = record->protocol->StartDuplex48k(channels);
    if (cfg != kIOReturnSuccess) {
        ASFW_LOG_ERROR(Audio, "DiceAudioBackend: StartDuplex48k failed GUID=%llx status=0x%x", guid, cfg);
        rollback();
        return cfg;
    }

    const kern_return_t krRx = isoch_.StartReceive(channels.deviceToHostIsoChannel,
                                                   hardware_,
                                                   rxMem,
                                                   rxBytes);
    if (krRx != kIOReturnSuccess) {
        ASFW_LOG_ERROR(Audio, "DiceAudioBackend: StartReceive failed GUID=%llx kr=0x%x", guid, krRx);
        rollback();
        return krRx;
    }

    const IOReturn armStatus = record->protocol->ArmDuplex48kAfterReceiveStart();
    if (armStatus != kIOReturnSuccess) {
        ASFW_LOG_ERROR(Audio,
                       "DiceAudioBackend: ArmDuplex48kAfterReceiveStart failed GUID=%llx status=0x%x",
                       guid,
                       armStatus);
        rollback();
        return armStatus;
    }

    IOBufferMemoryDescriptor* txMemRaw = nullptr;
    uint64_t txBytes = 0;
    const kern_return_t txCopy = nub->CopyTransmitQueueMemory(&txMemRaw, &txBytes);
    auto txMem = Common::AdoptRetained(txMemRaw);
    if (txCopy != kIOReturnSuccess || !txMem || txBytes == 0) {
        rollback();
        return (txCopy == kIOReturnSuccess) ? kIOReturnNoMemory : txCopy;
    }

    const uint8_t sid = ReadLocalSid(hardware_);
    const uint32_t streamModeRaw = static_cast<uint32_t>(Model::StreamMode::kBlocking);
    const Encoding::AudioWireFormat wireFormat = ResolveDicePlaybackWireFormat(*record, caps);
    const kern_return_t krTx = isoch_.StartTransmit(channels.hostToDeviceIsoChannel,
                                                    hardware_,
                                                    sid,
                                                    streamModeRaw,
                                                    caps.hostOutputPcmChannels,
                                                    caps.hostToDeviceAm824Slots,
                                                    wireFormat,
                                                    txMem,
                                                    txBytes,
                                                    nullptr,
                                                    0,
                                                    0);
    if (krTx != kIOReturnSuccess) {
        ASFW_LOG_ERROR(Audio, "DiceAudioBackend: StartTransmit failed GUID=%llx kr=0x%x", guid, krTx);
        rollback();
        return krTx;
    }

    const IOReturn completion = record->protocol->CompleteDuplex48kStart();
    if (completion != kIOReturnSuccess) {
        ASFW_LOG_ERROR(Audio,
                       "DiceAudioBackend: CompleteDuplex48kStart failed GUID=%llx status=0x%x",
                       guid,
                       completion);
        rollback();
        return completion;
    }

    return kIOReturnSuccess;
}

IOReturn DiceAudioBackend::StopStreaming(uint64_t guid) noexcept {
    if (guid == 0) return kIOReturnBadArgument;
    const kern_return_t stopKr = isoch_.StopDuplex(guid);

    if (auto* record = registry_.FindByGuid(guid); record && record->protocol) {
        const IOReturn protocolStop = record->protocol->StopDuplex();
        if (protocolStop != kIOReturnSuccess && protocolStop != kIOReturnUnsupported) {
            ASFW_LOG_ERROR(Audio,
                           "DiceAudioBackend: StopDuplex failed GUID=%llx status=0x%x",
                           guid,
                           protocolStop);
            if (stopKr == kIOReturnSuccess) {
                return protocolStop;
            }
        }
    }

    return stopKr;
}

} // namespace ASFW::Audio
