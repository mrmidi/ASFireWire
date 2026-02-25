// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2026 ASFireWire Project

#include "AVCAudioBackend.hpp"

#include "../../Logging/Logging.hpp"

#include <DriverKit/IOLib.h>
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <DriverKit/OSSharedPtr.h>
#include <net.mrmidi.ASFW.ASFWDriver/ASFWAudioNub.h>

namespace ASFW::Audio {

namespace {

constexpr uint8_t kDefaultIrChannel = 0;
constexpr uint8_t kDefaultItChannel = 1;

inline uint8_t ReadLocalSid(Driver::HardwareInterface& hw) noexcept {
    // OHCI NodeID register: low 6 bits are node number.
    return static_cast<uint8_t>(hw.ReadNodeID() & 0x3Fu);
}

} // namespace

AVCAudioBackend::AVCAudioBackend(AudioNubPublisher& publisher,
                                 Discovery::DeviceRegistry& registry,
                                 Driver::IsochService& isoch,
                                 Driver::HardwareInterface& hardware) noexcept
    : publisher_(publisher)
    , registry_(registry)
    , isoch_(isoch)
    , hardware_(hardware) {
    lock_ = IOLockAlloc();
    if (!lock_) {
        ASFW_LOG_ERROR(Audio, "AVCAudioBackend: Failed to allocate lock");
    }
}

AVCAudioBackend::~AVCAudioBackend() noexcept {
    if (lock_) {
        IOLockFree(lock_);
        lock_ = nullptr;
    }
}

void AVCAudioBackend::OnAudioConfigurationReady(uint64_t guid, const Model::ASFWAudioDevice& config) noexcept {
    if (guid == 0) return;

    if (lock_) {
        IOLockLock(lock_);
        configByGuid_[guid] = config;
        IOLockUnlock(lock_);
    }

    (void)publisher_.EnsureNub(guid, config, "AVC");
}

void AVCAudioBackend::OnDeviceRemoved(uint64_t guid) noexcept {
    if (guid == 0) return;

    (void)StopStreaming(guid);
    publisher_.TerminateNub(guid, "AVC-Removed");

    if (lock_) {
        IOLockLock(lock_);
        configByGuid_.erase(guid);
        IOLockUnlock(lock_);
    }
}

bool AVCAudioBackend::WaitForCMP(std::atomic<bool>& done,
                                 std::atomic<ASFW::CMP::CMPStatus>& status,
                                 uint32_t timeoutMs) noexcept {
    constexpr uint32_t kPollMs = 5;
    for (uint32_t waited = 0; waited < timeoutMs; waited += kPollMs) {
        if (done.load(std::memory_order_acquire)) {
            return status.load(std::memory_order_acquire) == ASFW::CMP::CMPStatus::Success;
        }
        IOSleep(kPollMs);
    }
    return false;
}

IOReturn AVCAudioBackend::StartStreaming(uint64_t guid) noexcept {
    if (guid == 0) return kIOReturnBadArgument;

    if (!cmpClient_) {
        ASFW_LOG(Audio, "AVCAudioBackend: StartStreaming not ready (CMPClient missing)");
        return kIOReturnNotReady;
    }

    Model::ASFWAudioDevice config{};
    bool hasConfig = false;
    if (lock_) {
        IOLockLock(lock_);
        auto it = configByGuid_.find(guid);
        if (it != configByGuid_.end()) {
            config = it->second;
            hasConfig = true;
        }
        IOLockUnlock(lock_);
    }
    if (!hasConfig) {
        ASFW_LOG(Audio, "AVCAudioBackend: StartStreaming not ready (no config) GUID=0x%016llx", guid);
        return kIOReturnNotReady;
    }

    const auto* record = registry_.FindByGuid(guid);
    if (!record) {
        ASFW_LOG(Audio, "AVCAudioBackend: StartStreaming not ready (no device record) GUID=0x%016llx", guid);
        return kIOReturnNotReady;
    }

    // CMP targets PCR space on the remote device (AV/C family policy).
    cmpClient_->SetDeviceNode(static_cast<uint8_t>(record->nodeId),
                              static_cast<ASFW::IRM::Generation>(record->gen));

    auto* nub = publisher_.GetNub(guid);
    if (!nub) {
        (void)publisher_.EnsureNub(guid, config, "AVC-Start");
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

    // Start IR first so capture packets don't get dropped.
    {
        const kern_return_t krRx = isoch_.StartReceive(kDefaultIrChannel,
                                                       hardware_,
                                                       rxMem.detach(),
                                                       rxBytes);
        if (krRx != kIOReturnSuccess) {
            ASFW_LOG_ERROR(Audio, "AVCAudioBackend: StartReceive failed GUID=0x%016llx kr=0x%x", guid, krRx);
            return krRx;
        }
    }

    // CMP connect oPCR[0] (device->host).
    {
        std::atomic<bool> done{false};
        std::atomic<ASFW::CMP::CMPStatus> status{ASFW::CMP::CMPStatus::Failed};
        cmpClient_->ConnectOPCR(0, [&done, &status](ASFW::CMP::CMPStatus s) {
            status.store(s, std::memory_order_release);
            done.store(true, std::memory_order_release);
        });

        if (!WaitForCMP(done, status, 250)) {
            const auto s = status.load(std::memory_order_acquire);
            ASFW_LOG_ERROR(Audio,
                           "AVCAudioBackend: CMP ConnectOPCR failed GUID=0x%016llx status=%{public}s(%d)",
                           guid,
                           ASFW::IRM::ToString(s),
                           static_cast<int>(s));
            (void)isoch_.StopReceive();
            return kIOReturnError;
        }
    }

    // Start IT transport (host->device) and then connect iPCR[0].
    {
        const uint8_t sid = ReadLocalSid(hardware_);
        const uint32_t streamModeRaw = static_cast<uint32_t>(config.streamMode);

        // AV/C playback streams normally have PCM-only wire slots.
        const uint32_t am824Slots = config.outputChannelCount;

        OSSharedPtr<IOBufferMemoryDescriptor> txMem{};
        uint64_t txBytes = 0;
        const kern_return_t txCopy = nub->CopyTransmitQueueMemory(txMem.attach(), &txBytes);
        if (txCopy != kIOReturnSuccess || !txMem || txBytes == 0) {
            (void)isoch_.StopReceive();
            // Best-effort: disconnect oPCR.
            cmpClient_->DisconnectOPCR(0, [](ASFW::CMP::CMPStatus) {});
            return (txCopy == kIOReturnSuccess) ? kIOReturnNoMemory : txCopy;
        }

        const kern_return_t krTx = isoch_.StartTransmit(kDefaultItChannel,
                                                        hardware_,
                                                        sid,
                                                        streamModeRaw,
                                                        config.outputChannelCount,
                                                        am824Slots,
                                                        txMem.detach(),
                                                        txBytes,
                                                        nullptr,
                                                        0,
                                                        0);
        if (krTx != kIOReturnSuccess) {
            ASFW_LOG_ERROR(Audio, "AVCAudioBackend: StartTransmit failed GUID=0x%016llx kr=0x%x", guid, krTx);
            (void)isoch_.StopReceive();
            // Best-effort: disconnect oPCR.
            cmpClient_->DisconnectOPCR(0, [](ASFW::CMP::CMPStatus) {});
            return krTx;
        }

        std::atomic<bool> done{false};
        std::atomic<ASFW::CMP::CMPStatus> status{ASFW::CMP::CMPStatus::Failed};
        cmpClient_->ConnectIPCR(0, kDefaultItChannel, [&done, &status](ASFW::CMP::CMPStatus s) {
            status.store(s, std::memory_order_release);
            done.store(true, std::memory_order_release);
        });

        if (!WaitForCMP(done, status, 250)) {
            const auto s = status.load(std::memory_order_acquire);
            ASFW_LOG_ERROR(Audio,
                           "AVCAudioBackend: CMP ConnectIPCR failed GUID=0x%016llx status=%{public}s(%d)",
                           guid,
                           ASFW::IRM::ToString(s),
                           static_cast<int>(s));
            (void)isoch_.StopTransmit();
            (void)isoch_.StopReceive();
            cmpClient_->DisconnectOPCR(0, [](ASFW::CMP::CMPStatus) {});
            return kIOReturnError;
        }
    }

    ASFW_LOG(Audio,
             "AVCAudioBackend: Streaming started GUID=0x%016llx (in=%u out=%u mode=%{public}s)",
             guid,
             config.inputChannelCount,
             config.outputChannelCount,
             config.streamMode == Model::StreamMode::kBlocking ? "blocking" : "non-blocking");

    return kIOReturnSuccess;
}

IOReturn AVCAudioBackend::StopStreaming(uint64_t guid) noexcept {
    if (guid == 0) return kIOReturnBadArgument;

    // Stop transport regardless of CMP availability (best-effort).
    if (!cmpClient_) {
        (void)isoch_.StopTransmit();
        (void)isoch_.StopReceive();
        return kIOReturnSuccess;
    }

    const auto* record = registry_.FindByGuid(guid);
    if (record) {
        cmpClient_->SetDeviceNode(static_cast<uint8_t>(record->nodeId),
                                  static_cast<ASFW::IRM::Generation>(record->gen));
    }

    // Disconnect iPCR first (host->device), then stop IT.
    {
        std::atomic<bool> done{false};
        std::atomic<ASFW::CMP::CMPStatus> status{ASFW::CMP::CMPStatus::Failed};
        cmpClient_->DisconnectIPCR(0, [&done, &status](ASFW::CMP::CMPStatus s) {
            status.store(s, std::memory_order_release);
            done.store(true, std::memory_order_release);
        });
        (void)WaitForCMP(done, status, 250);
    }

    (void)isoch_.StopTransmit();

    // Disconnect oPCR, then stop IR.
    {
        std::atomic<bool> done{false};
        std::atomic<ASFW::CMP::CMPStatus> status{ASFW::CMP::CMPStatus::Failed};
        cmpClient_->DisconnectOPCR(0, [&done, &status](ASFW::CMP::CMPStatus s) {
            status.store(s, std::memory_order_release);
            done.store(true, std::memory_order_release);
        });
        (void)WaitForCMP(done, status, 250);
    }

    (void)isoch_.StopReceive();

    ASFW_LOG(Audio, "AVCAudioBackend: Streaming stopped GUID=0x%016llx", guid);
    return kIOReturnSuccess;
}

} // namespace ASFW::Audio
