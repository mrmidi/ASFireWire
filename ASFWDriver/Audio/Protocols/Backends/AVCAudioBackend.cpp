// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2026 ASFireWire Project

#include "AVCAudioBackend.hpp"

#include "../../../Audio/Core/AudioEndpointRuntime.hpp"
#include "../../../Audio/Core/AudioRuntimeRegistry.hpp"
#include "../../../Common/DriverKitOwnership.hpp"
#include "../../../Logging/Logging.hpp"

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
                                 AudioRuntimeRegistry& runtime,
                                 Driver::IsochService& isoch,
                                 Driver::HardwareInterface& hardware) noexcept
    : publisher_(publisher)
    , registry_(registry)
    , runtime_(runtime)
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

    bool shouldStop = false;
    if (lock_) {
        IOLockLock(lock_);
        shouldStop = (activeGuid_ == guid);
        IOLockUnlock(lock_);
    }

    if (shouldStop) {
        (void)StopStreaming(guid);
    } else {
        ASFW_LOG(Audio,
                 "AVCAudioBackend: OnDeviceRemoved skipping stop for inactive GUID=0x%016llx",
                 guid);
    }
    publisher_.TerminateNub(guid, "AVC-Removed");

    if (lock_) {
        IOLockLock(lock_);
        configByGuid_.erase(guid);
        if (activeGuid_ == guid) {
            activeGuid_ = 0;
        }
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

    if (lock_) {
        IOLockLock(lock_);
        if (activeGuid_ != 0 && activeGuid_ != guid) {
            const uint64_t active = activeGuid_;
            IOLockUnlock(lock_);
            ASFW_LOG_WARNING(Audio,
                             "AVCAudioBackend: StartStreaming busy requested=0x%016llx active=0x%016llx",
                             guid,
                             active);
            return kIOReturnBusy;
        }
        activeGuid_ = guid;
        IOLockUnlock(lock_);
    }

    auto failStart = [&](IOReturn status, const char* stage) -> IOReturn {
        if (lock_) {
            IOLockLock(lock_);
            if (activeGuid_ == guid) {
                activeGuid_ = 0;
            }
            IOLockUnlock(lock_);
        }
        ASFW_LOG_ERROR(Audio,
                       "AVCAudioBackend: StartStreaming failed stage=%{public}s GUID=0x%016llx kr=0x%x",
                       stage ? stage : "unknown",
                       guid,
                       status);
        return status;
    };

    if (!cmpClient_) {
        ASFW_LOG(Audio, "AVCAudioBackend: StartStreaming not ready (CMPClient missing)");
        return failStart(kIOReturnNotReady, "CMPClient");
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
        return failStart(kIOReturnNotReady, "config");
    }

    const auto* record = registry_.FindByGuid(guid);
    if (!record) {
        ASFW_LOG(Audio, "AVCAudioBackend: StartStreaming not ready (no device record) GUID=0x%016llx", guid);
        return failStart(kIOReturnNotReady, "device record");
    }

    // CMP targets PCR space on the remote device (AV/C family policy).
    cmpClient_->SetDeviceNode(static_cast<uint8_t>(record->nodeId),
                              static_cast<ASFW::IRM::Generation>(record->gen));

    auto* nub = publisher_.GetNub(guid);
    if (!nub) {
        (void)publisher_.EnsureNub(guid, config, "AVC-Start");
        nub = publisher_.GetNub(guid);
        if (!nub) return failStart(kIOReturnNotReady, "nub");
    }

    auto endpoint = runtime_.FindEndpointRuntime(guid);
    auto* bindingSource = endpoint.get();
    if (!bindingSource) {
        return failStart(kIOReturnNotReady, "direct binding source");
    }
    if (!endpoint->HasCompleteDirectAudioMemory()) {
        return failStart(kIOReturnNotReady, "direct memory");
    }

    // Start IR first so capture packets don't get dropped.
    {
        const kern_return_t krRx = isoch_.StartReceive(kDefaultIrChannel,
                                                       hardware_,
                                                       bindingSource);
        if (krRx != kIOReturnSuccess) {
            ASFW_LOG_ERROR(Audio, "AVCAudioBackend: StartReceive failed GUID=0x%016llx kr=0x%x", guid, krRx);
            return failStart(krRx, "StartReceive");
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
            return failStart(kIOReturnError, "ConnectOPCR");
        }
    }

    // Start IT transport (host->device) and then connect iPCR[0].
    {
        const uint8_t sid = ReadLocalSid(hardware_);

        const kern_return_t krTx = isoch_.StartTransmit(kDefaultItChannel,
                                                        hardware_,
                                                        sid);
        if (krTx != kIOReturnSuccess) {
            ASFW_LOG_ERROR(Audio, "AVCAudioBackend: StartTransmit failed GUID=0x%016llx kr=0x%x", guid, krTx);
            (void)isoch_.StopReceive();
            // Best-effort: disconnect oPCR.
            cmpClient_->DisconnectOPCR(0, [](ASFW::CMP::CMPStatus) { /* best-effort, result ignored */ });
            return failStart(krTx, "StartTransmit");
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
            cmpClient_->DisconnectOPCR(0, [](ASFW::CMP::CMPStatus) { /* best-effort, result ignored */ });
            return failStart(kIOReturnError, "ConnectIPCR");
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

    if (lock_) {
        IOLockLock(lock_);
        if (activeGuid_ != 0 && activeGuid_ != guid) {
            const uint64_t active = activeGuid_;
            IOLockUnlock(lock_);
            ASFW_LOG_WARNING(Audio,
                             "AVCAudioBackend: StopStreaming refused requested=0x%016llx active=0x%016llx",
                             guid,
                             active);
            return kIOReturnBusy;
        }
        if (activeGuid_ == 0) {
            IOLockUnlock(lock_);
            ASFW_LOG(Audio,
                     "AVCAudioBackend: StopStreaming idempotent inactive GUID=0x%016llx",
                     guid);
            return kIOReturnSuccess;
        }
        IOLockUnlock(lock_);
    }

    // Stop transport regardless of CMP availability (best-effort).
    if (!cmpClient_) {
        (void)isoch_.StopTransmit();
        (void)isoch_.StopReceive();
        if (lock_) {
            IOLockLock(lock_);
            if (activeGuid_ == guid) {
                activeGuid_ = 0;
            }
            IOLockUnlock(lock_);
        }
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

    if (lock_) {
        IOLockLock(lock_);
        if (activeGuid_ == guid) {
            activeGuid_ = 0;
        }
        IOLockUnlock(lock_);
    }

    ASFW_LOG(Audio, "AVCAudioBackend: Streaming stopped GUID=0x%016llx", guid);
    return kIOReturnSuccess;
}

} // namespace ASFW::Audio
