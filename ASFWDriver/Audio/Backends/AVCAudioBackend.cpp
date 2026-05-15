// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2026 ASFireWire Project

#include "AVCAudioBackend.hpp"

#include "../../Common/DriverKitOwnership.hpp"
#include "../../Logging/Logging.hpp"

#include <DriverKit/IOLib.h>
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <DriverKit/OSSharedPtr.h>
#include <net.mrmidi.ASFW.ASFWDriver/ASFWAudioNub.h>

namespace ASFW::Audio {

namespace {

// Candidate channels to try when IRM allocation is requested.
// Try low-numbered channels first (most devices use 0/1).
constexpr uint8_t kCandidateIrChannel = 0;
constexpr uint8_t kCandidateItChannel = 1;

constexpr uint32_t kIRMTimeoutMs = 500;

inline uint8_t ReadLocalSid(Driver::HardwareInterface& hw) noexcept {
    // OHCI NodeID register: low 6 bits are node number.
    return static_cast<uint8_t>(hw.ReadNodeID() & 0x3Fu);
}

// Compute IRM bandwidth units for an isochronous audio stream.
// Uses AM824 quadlet size (32 bits/sample) at S400.
inline uint32_t ComputeBandwidth(uint32_t channelCount, uint32_t sampleRateHz) noexcept {
    const uint32_t bps = channelCount * sampleRateHz * 32u;
    return IRM::CalculateBandwidthUnits({bps, 400u, 10u});
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

bool AVCAudioBackend::WaitForIRM(std::atomic<bool>& done,
                                 std::atomic<ASFW::IRM::AllocationStatus>& status,
                                 uint32_t timeoutMs) noexcept {
    constexpr uint32_t kPollMs = 5;
    for (uint32_t waited = 0; waited < timeoutMs; waited += kPollMs) {
        if (done.load(std::memory_order_acquire)) {
            return status.load(std::memory_order_acquire) == IRM::AllocationStatus::Success;
        }
        IOSleep(kPollMs);
    }
    return false;
}

IOReturn AVCAudioBackend::AllocateIRMResources(uint8_t irChannel,
                                               uint8_t itChannel,
                                               uint32_t irBandwidth,
                                               uint32_t itBandwidth) noexcept {
    // Allocate IR channel + bandwidth atomically.
    {
        std::atomic<bool> done{false};
        std::atomic<IRM::AllocationStatus> status{IRM::AllocationStatus::Failed};
        irmClient_->AllocateResources(irChannel, irBandwidth,
            [&done, &status](IRM::AllocationStatus s) {
                status.store(s, std::memory_order_release);
                done.store(true, std::memory_order_release);
            });
        if (!WaitForIRM(done, status, kIRMTimeoutMs)) {
            const auto s = status.load(std::memory_order_acquire);
            ASFW_LOG_ERROR(Audio,
                           "AVCAudioBackend: IRM AllocateResources IR ch=%u failed: %{public}s",
                           irChannel, IRM::ToString(s));
            return kIOReturnError;
        }
    }

    // Allocate IT channel + bandwidth atomically.
    {
        std::atomic<bool> done{false};
        std::atomic<IRM::AllocationStatus> status{IRM::AllocationStatus::Failed};
        irmClient_->AllocateResources(itChannel, itBandwidth,
            [&done, &status](IRM::AllocationStatus s) {
                status.store(s, std::memory_order_release);
                done.store(true, std::memory_order_release);
            });
        if (!WaitForIRM(done, status, kIRMTimeoutMs)) {
            const auto s = status.load(std::memory_order_acquire);
            ASFW_LOG_ERROR(Audio,
                           "AVCAudioBackend: IRM AllocateResources IT ch=%u failed: %{public}s",
                           itChannel, IRM::ToString(s));
            // Roll back IR allocation.
            irmClient_->ReleaseResources(irChannel, irBandwidth,
                [](IRM::AllocationStatus) { /* best-effort */ });
            return kIOReturnError;
        }
    }

    allocated_ = {irChannel, itChannel, irBandwidth, itBandwidth, true};
    ASFW_LOG(Audio, "AVCAudioBackend: IRM allocated IR ch=%u IT ch=%u (irBW=%u itBW=%u units)",
             irChannel, itChannel, irBandwidth, itBandwidth);
    return kIOReturnSuccess;
}

void AVCAudioBackend::ReleaseIRMResources() noexcept {
    if (!allocated_.valid || !irmClient_) {
        allocated_ = {};
        return;
    }
    irmClient_->ReleaseResources(allocated_.irChannel, allocated_.irBandwidth,
        [](IRM::AllocationStatus) { /* best-effort */ });
    irmClient_->ReleaseResources(allocated_.itChannel, allocated_.itBandwidth,
        [](IRM::AllocationStatus) { /* best-effort */ });
    ASFW_LOG(Audio, "AVCAudioBackend: IRM released IR ch=%u IT ch=%u",
             allocated_.irChannel, allocated_.itChannel);
    allocated_ = {};
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

    // IRM: allocate isochronous channels and bandwidth before touching PCR or DMA.
    const uint32_t sampleRateHz = config.currentSampleRate > 0
                                      ? static_cast<uint32_t>(config.currentSampleRate)
                                      : 48000u;
    const uint32_t irBW = ComputeBandwidth(config.inputChannelCount, sampleRateHz);
    const uint32_t itBW = ComputeBandwidth(config.outputChannelCount, sampleRateHz);

    if (irmClient_) {
        const IOReturn krIRM = AllocateIRMResources(kCandidateIrChannel, kCandidateItChannel,
                                                    irBW, itBW);
        if (krIRM != kIOReturnSuccess) {
            return krIRM;
        }
    } else {
        // No IRM client — fall back to candidate channels without reservation.
        // This violates IEEE 1394 but allows operation on a single-device bus.
        ASFW_LOG_WARNING(Audio,
                         "AVCAudioBackend: IRMClient not set — using ch=%u/%u without IRM reservation",
                         kCandidateIrChannel, kCandidateItChannel);
        allocated_ = {kCandidateIrChannel, kCandidateItChannel, 0u, 0u, false};
    }

    const uint8_t irChannel = allocated_.irChannel;
    const uint8_t itChannel = allocated_.itChannel;

    // CMP targets PCR space on the remote device (AV/C family policy).
    cmpClient_->SetDeviceNode(static_cast<uint8_t>(record->nodeId),
                              static_cast<ASFW::IRM::Generation>(record->gen));

    auto* nub = publisher_.GetNub(guid);
    if (!nub) {
        (void)publisher_.EnsureNub(guid, config, "AVC-Start");
        nub = publisher_.GetNub(guid);
        if (!nub) {
            ReleaseIRMResources();
            return kIOReturnNotReady;
        }
    }

    // Ensure queues exist before wiring to isoch contexts.
    nub->EnsureRxQueueCreated();
    IOBufferMemoryDescriptor* rxMemRaw = nullptr;
    uint64_t rxBytes = 0;
    const kern_return_t rxCopy = nub->CopyRxQueueMemory(&rxMemRaw, &rxBytes);
    auto rxMem = Common::AdoptRetained(rxMemRaw);
    if (rxCopy != kIOReturnSuccess || !rxMem || rxBytes == 0) {
        ReleaseIRMResources();
        return (rxCopy == kIOReturnSuccess) ? kIOReturnNoMemory : rxCopy;
    }

    // Start IR first so capture packets don't get dropped.
    {
        const kern_return_t krRx = isoch_.StartReceive(irChannel,
                                                       hardware_,
                                                       rxMem,
                                                       rxBytes);
        if (krRx != kIOReturnSuccess) {
            ASFW_LOG_ERROR(Audio, "AVCAudioBackend: StartReceive failed GUID=0x%016llx kr=0x%x", guid, krRx);
            ReleaseIRMResources();
            return krRx;
        }
    }

    // CMP connect oPCR[0] (device->host) — writes irChannel into oPCR per IEC 61883-1 §10.4.2.
    {
        std::atomic<bool> done{false};
        std::atomic<ASFW::CMP::CMPStatus> status{ASFW::CMP::CMPStatus::Failed};
        cmpClient_->ConnectOPCR(0, irChannel, [&done, &status](ASFW::CMP::CMPStatus s) {
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
            ReleaseIRMResources();
            return kIOReturnError;
        }
    }

    // Read-back oPCR[0] to confirm the channel was written correctly (per Apple IOFireWireAVC pattern).
    {
        std::atomic<bool> done{false};
        std::atomic<bool> readOk{false};
        std::atomic<uint32_t> opcrValue{0};
        cmpClient_->ReadOPCR(0, [&done, &readOk, &opcrValue](bool success, uint32_t value) {
            readOk.store(success, std::memory_order_release);
            opcrValue.store(value, std::memory_order_release);
            done.store(true, std::memory_order_release);
        });

        constexpr uint32_t kPollMs = 5;
        for (uint32_t waited = 0; waited < 250u && !done.load(std::memory_order_acquire); waited += kPollMs) {
            IOSleep(kPollMs);
        }

        if (done.load(std::memory_order_acquire) && readOk.load(std::memory_order_acquire)) {
            const uint8_t confirmedCh = ASFW::CMP::PCRBits::GetChannel(opcrValue.load(std::memory_order_acquire));
            if (confirmedCh != irChannel) {
                ASFW_LOG_WARNING(Audio,
                    "AVCAudioBackend: oPCR[0] channel mismatch: expected=%u actual=%u",
                    irChannel, confirmedCh);
            } else {
                ASFW_LOG(Audio, "AVCAudioBackend: oPCR[0] read-back confirmed channel=%u", confirmedCh);
            }
        } else {
            ASFW_LOG_WARNING(Audio, "AVCAudioBackend: oPCR[0] read-back failed or timed out (continuing)");
        }
    }

    // Start IT transport (host->device) and then connect iPCR[0].
    {
        const uint8_t sid = ReadLocalSid(hardware_);
        const uint32_t streamModeRaw = static_cast<uint32_t>(config.streamMode);

        // AV/C playback streams normally have PCM-only wire slots.
        const uint32_t am824Slots = config.outputChannelCount;

        IOBufferMemoryDescriptor* txMemRaw = nullptr;
        uint64_t txBytes = 0;
        const kern_return_t txCopy = nub->CopyTransmitQueueMemory(&txMemRaw, &txBytes);
        auto txMem = Common::AdoptRetained(txMemRaw);
        if (txCopy != kIOReturnSuccess || !txMem || txBytes == 0) {
            (void)isoch_.StopReceive();
            cmpClient_->DisconnectOPCR(0, [](ASFW::CMP::CMPStatus) { /* best-effort */ });
            ReleaseIRMResources();
            return (txCopy == kIOReturnSuccess) ? kIOReturnNoMemory : txCopy;
        }

        const kern_return_t krTx = isoch_.StartTransmit(itChannel,
                                                        hardware_,
                                                        sid,
                                                        streamModeRaw,
                                                        config.outputChannelCount,
                                                        am824Slots,
                                                        txMem,
                                                        txBytes,
                                                        nullptr,
                                                        0,
                                                        0);
        if (krTx != kIOReturnSuccess) {
            ASFW_LOG_ERROR(Audio, "AVCAudioBackend: StartTransmit failed GUID=0x%016llx kr=0x%x", guid, krTx);
            (void)isoch_.StopReceive();
            cmpClient_->DisconnectOPCR(0, [](ASFW::CMP::CMPStatus) { /* best-effort */ });
            ReleaseIRMResources();
            return krTx;
        }

        std::atomic<bool> done{false};
        std::atomic<ASFW::CMP::CMPStatus> status{ASFW::CMP::CMPStatus::Failed};
        cmpClient_->ConnectIPCR(0, itChannel, [&done, &status](ASFW::CMP::CMPStatus s) {
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
            cmpClient_->DisconnectOPCR(0, [](ASFW::CMP::CMPStatus) { /* best-effort */ });
            ReleaseIRMResources();
            return kIOReturnError;
        }
    }

    ASFW_LOG(Audio,
             "AVCAudioBackend: Streaming started GUID=0x%016llx irCh=%u itCh=%u (in=%u out=%u mode=%{public}s)",
             guid,
             irChannel,
             itChannel,
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
        ReleaseIRMResources();
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

    ReleaseIRMResources();

    ASFW_LOG(Audio, "AVCAudioBackend: Streaming stopped GUID=0x%016llx", guid);
    return kIOReturnSuccess;
}

} // namespace ASFW::Audio
