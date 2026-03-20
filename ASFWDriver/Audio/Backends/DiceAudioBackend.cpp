// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2026 ASFireWire Project

#include "DiceAudioBackend.hpp"
#include "DiceBringupSequence.hpp"

#include "../../Common/DriverKitOwnership.hpp"
#include "../../IRM/IRMClient.hpp"
#include "../../Logging/Logging.hpp"
#include "../../Protocols/Audio/DICE/TCAT/DICEKnownProfiles.hpp"
#include "../../Protocols/Audio/DeviceProtocolFactory.hpp"

#include <DriverKit/IOLib.h>
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <DriverKit/OSSharedPtr.h>
#include <net.mrmidi.ASFW.ASFWDriver/ASFWAudioNub.h>
#include <atomic>
#include <functional>
#include <memory>
#include <string>

namespace ASFW::Audio {

namespace {

// Sync bridge for async protocol calls. This is the ONE place IOSleep remains.
struct SyncVoidState {
    std::atomic<bool> done{false};
    std::atomic<IOReturn> status{kIOReturnTimeout};
};

constexpr uint32_t kSyncBridgeTimeoutMs = 5000;
constexpr uint32_t kSyncBridgePollMs = 10;
// DICE raw-parity bring-up now includes a 5s CLOCK_ACCEPTED wait plus
// several preflight transactions before the controller can report success.
// Keep extra headroom here so the backend does not time out and force
// rollback while the controller is still legitimately progressing.
constexpr uint32_t kDicePrepareTimeoutMs = 12000;

template <typename StartFn>
IOReturn WaitForVoid(StartFn&& fn, uint32_t timeoutMs = kSyncBridgeTimeoutMs) {
    auto state = std::make_shared<SyncVoidState>();
    fn([state](IOReturn status) {
        state->status.store(status, std::memory_order_relaxed);
        state->done.store(true, std::memory_order_release);
    });

    for (uint32_t waited = 0; waited < timeoutMs; waited += kSyncBridgePollMs) {
        if (state->done.load(std::memory_order_acquire)) {
            return state->status.load(std::memory_order_relaxed);
        }
        IOSleep(kSyncBridgePollMs);
    }

    return state->done.load(std::memory_order_acquire)
        ? state->status.load(std::memory_order_relaxed)
        : kIOReturnTimeout;
}

// Match the clean original saffire.kext bring-up on SPro24DSP:
// DICE RX (host->device playback) uses isoch channel 0 and
// DICE TX (device->host capture) uses isoch channel 1.
constexpr uint8_t kDefaultIrChannel = 1;
constexpr uint8_t kDefaultItChannel = 0;
constexpr uint32_t kPlaybackBandwidthUnits = 320;
constexpr uint32_t kCaptureBandwidthUnits = 576;

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
        if (DICE::TCAT::TryGetKnownDICEProfile(record->vendorId, record->modelId, caps)) {
            ASFW_LOG_WARNING(Audio,
                             "DiceAudioBackend: runtime caps not ready for GUID=%llx; using known DICE profile %u/%u",
                             guid,
                             caps.hostInputPcmChannels,
                             caps.hostOutputPcmChannels);
        } else {
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
    if (!record->protocol->GetRuntimeAudioStreamCaps(caps) &&
        !DICE::TCAT::TryGetKnownDICEProfile(record->vendorId, record->modelId, caps)) {
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

    auto* irmClient = record->protocol->GetIRMClient();
    if (irmClient == nullptr) {
        ASFW_LOG_ERROR(Audio, "DiceAudioBackend: protocol missing IRM client GUID=%llx", guid);
        return kIOReturnNotReady;
    }

    const kern_return_t claimStatus = isoch_.BeginSplitDuplex(guid);
    if (claimStatus != kIOReturnSuccess) {
        ASFW_LOG_ERROR(Audio,
                       "DiceAudioBackend: BeginSplitDuplex failed GUID=%llx kr=0x%x",
                       guid,
                       claimStatus);
        return claimStatus;
    }

    const auto rollback = [this, record, guid, irmClient]() {
        const kern_return_t stopKr = isoch_.StopDuplex(guid, irmClient);
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

    return Detail::RunDiceBringupSequence(
        [&]() -> IOReturn {
            const IOReturn cfg = WaitForVoid([&](auto cb) {
                record->protocol->PrepareDuplex48k(channels, std::move(cb));
            }, kDicePrepareTimeoutMs);
            if (cfg != kIOReturnSuccess) {
                ASFW_LOG_ERROR(Audio,
                               "DiceAudioBackend: PrepareDuplex48k failed GUID=%llx status=0x%x",
                               guid,
                               cfg);
            }
            return cfg;
        },
        [&]() -> IOReturn {
            const kern_return_t reserveStatus =
                isoch_.ReservePlaybackResources(guid,
                                                *irmClient,
                                                channels.hostToDeviceIsoChannel,
                                                kPlaybackBandwidthUnits);
            if (reserveStatus != kIOReturnSuccess) {
                ASFW_LOG_ERROR(Audio,
                               "DiceAudioBackend: ReservePlaybackResources failed GUID=%llx kr=0x%x",
                               guid,
                               reserveStatus);
            }
            return reserveStatus;
        },
        [&]() -> IOReturn {
            const IOReturn programRxStatus = WaitForVoid([&](auto cb) {
                record->protocol->ProgramRxForDuplex48k(std::move(cb));
            });
            if (programRxStatus != kIOReturnSuccess) {
                ASFW_LOG_ERROR(Audio,
                               "DiceAudioBackend: ProgramRxForDuplex48k failed GUID=%llx status=0x%x",
                               guid,
                               programRxStatus);
            }
            return programRxStatus;
        },
        [&]() -> IOReturn {
            const kern_return_t reserveStatus =
                isoch_.ReserveCaptureResources(guid,
                                               *irmClient,
                                               channels.deviceToHostIsoChannel,
                                               kCaptureBandwidthUnits);
            if (reserveStatus != kIOReturnSuccess) {
                ASFW_LOG_ERROR(Audio,
                               "DiceAudioBackend: ReserveCaptureResources failed GUID=%llx kr=0x%x",
                               guid,
                               reserveStatus);
            }
            return reserveStatus;
        },
        [&]() -> IOReturn {
            const kern_return_t krRx = isoch_.StartReceive(channels.deviceToHostIsoChannel,
                                                           hardware_,
                                                           rxMem,
                                                           rxBytes);
            if (krRx != kIOReturnSuccess) {
                ASFW_LOG_ERROR(Audio,
                               "DiceAudioBackend: StartReceive failed GUID=%llx kr=0x%x",
                               guid,
                               krRx);
            }
            return krRx;
        },
        [&]() -> IOReturn {
            const IOReturn txEnableStatus = WaitForVoid([&](auto cb) {
                record->protocol->ProgramTxAndEnableDuplex48k(std::move(cb));
            });
            if (txEnableStatus != kIOReturnSuccess) {
                ASFW_LOG_ERROR(Audio,
                               "DiceAudioBackend: ProgramTxAndEnableDuplex48k failed GUID=%llx status=0x%x",
                               guid,
                               txEnableStatus);
            }
            return txEnableStatus;
        },
        [&]() -> IOReturn {
            IOBufferMemoryDescriptor* txMemRaw = nullptr;
            uint64_t txBytes = 0;
            const kern_return_t txCopy = nub->CopyTransmitQueueMemory(&txMemRaw, &txBytes);
            auto txMem = Common::AdoptRetained(txMemRaw);
            if (txCopy != kIOReturnSuccess || !txMem || txBytes == 0) {
                return (txCopy == kIOReturnSuccess) ? kIOReturnNoMemory : txCopy;
            }

            const uint8_t sid = ReadLocalSid(hardware_);
            const uint32_t streamModeRaw = static_cast<uint32_t>(Model::StreamMode::kBlocking);
            const Encoding::AudioWireFormat wireFormat =
                ResolveDicePlaybackWireFormat(*record, caps);
            const kern_return_t krTx =
                isoch_.StartTransmit(channels.hostToDeviceIsoChannel,
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
                ASFW_LOG_ERROR(Audio,
                               "DiceAudioBackend: StartTransmit failed GUID=%llx kr=0x%x",
                               guid,
                               krTx);
            }
            return krTx;
        },
        [&]() -> IOReturn {
            const IOReturn completion = WaitForVoid([&](auto cb) {
                record->protocol->ConfirmDuplex48kStart(std::move(cb));
            });
            if (completion != kIOReturnSuccess) {
                ASFW_LOG_ERROR(Audio,
                               "DiceAudioBackend: ConfirmDuplex48kStart failed GUID=%llx status=0x%x",
                               guid,
                               completion);
            } else {
                EnsureNubForGuid(guid);
            }
            return completion;
        },
        rollback);
}

IOReturn DiceAudioBackend::StopStreaming(uint64_t guid) noexcept {
    if (guid == 0) return kIOReturnBadArgument;
    auto* irmClient = [&]() -> ::ASFW::IRM::IRMClient* {
        if (auto* record = registry_.FindByGuid(guid); record && record->protocol) {
            return record->protocol->GetIRMClient();
        }
        return nullptr;
    }();
    const kern_return_t stopKr = isoch_.StopDuplex(guid, irmClient);

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
