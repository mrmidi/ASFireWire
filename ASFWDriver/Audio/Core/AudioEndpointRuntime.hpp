// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#pragma once

#include "../DriverKit/Runtime/DirectAudioBindingSource.hpp"
#include "../Devices/ResolvedAudioEndpointProfile.hpp"
#include "../Runtime/AudioTelemetrySnapshot.hpp"
#include "../Config/AudioConstants.hpp"
#include "../Wire/AMDTP/AmdtpRateGeometry.hpp"
#include "../Shared/AudioGeometryResolver.hpp"
#include "../Shared/AudioTimingGeometry.hpp"
#include "../Runtime/TxLatencySession.hpp"
#include "../../UserClient/WireFormats/TxLatencySessionWireFormats.hpp"
#include "../../Logging/Logging.hpp"

#include <DriverKit/IOLib.h>

#if defined(ASFW_HOST_TEST)
#include "../../Testing/HostDriverKitStubs.hpp"
#else
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <DriverKit/IOMemoryDescriptor.h>
#include <DriverKit/IOMemoryMap.h>
#endif

#include <atomic>
#include <algorithm>
#include <cstring>
#include <cstdint>
#include <new>

namespace ASFW::Audio {

class AudioEndpointRuntime final : public Runtime::IDirectAudioBindingSource {
public:
    explicit AudioEndpointRuntime(
        const Devices::ResolvedAudioEndpointProfile& profile) noexcept
        : endpointId_(profile.endpointId),
          deviceInstanceId_(profile.deviceInstanceId),
          observedGuid_(profile.observedGuid),
          maximumOutputChannels_(MaximumPlaybackChannels(profile)),
          maximumInputChannels_(MaximumCaptureChannels(profile)),
          configuredOutputChannels_(PhysicalPlaybackChannels(profile.runtimeCaps)),
          configuredInputChannels_(PhysicalCaptureChannels(profile.runtimeCaps)),
          currentSampleRateHz_(profile.currentSampleRateHz),
          allocationLimits_(ComputeAllocationLimits(profile)),
          allocatedOutputBytes_(allocationLimits_.allocatedOutputBytes),
          allocatedInputBytes_(allocationLimits_.allocatedInputBytes),
          lock_(IOLockAlloc()) {}
    ~AudioEndpointRuntime() noexcept {
        ReleaseDirectAudioMemory();
        if (lock_) {
            IOLockFree(lock_);
            lock_ = nullptr;
        }
    }

    AudioEndpointRuntime(const AudioEndpointRuntime&) = delete;
    AudioEndpointRuntime& operator=(const AudioEndpointRuntime&) = delete;

    [[nodiscard]] Devices::AudioEndpointId EndpointId() const noexcept { return endpointId_; }
    [[nodiscard]] Discovery::DeviceInstanceId DeviceInstanceId() const noexcept {
        return deviceInstanceId_;
    }
    [[nodiscard]] uint64_t ObservedGuid() const noexcept { return observedGuid_; }
    [[nodiscard]] const Shared::DirectAudioAllocationLimits& AllocationLimits() const noexcept {
        return allocationLimits_;
    }
    [[nodiscard]] uint64_t AllocatedOutputBytes() const noexcept { return allocatedOutputBytes_; }
    [[nodiscard]] uint64_t AllocatedInputBytes() const noexcept { return allocatedInputBytes_; }
    [[nodiscard]] uint32_t DirectActiveOutputRingFrames() const noexcept {
        return directActiveOutputRingFrames_;
    }
    [[nodiscard]] uint32_t DirectActiveInputRingFrames() const noexcept {
        return directActiveInputRingFrames_;
    }

    [[nodiscard]] bool CopyActiveConfiguration(uint32_t& outSampleRateHz,
                                               uint32_t& outInputChannels,
                                               uint32_t& outOutputChannels,
                                               uint64_t& outTopologyRevision) const noexcept {
        if (!lock_) return false;
        IOLockLock(lock_);
        outSampleRateHz = currentSampleRateHz_;
        outInputChannels = configuredInputChannels_;
        outOutputChannels = configuredOutputChannels_;
        outTopologyRevision = topologyRevision_;
        IOLockUnlock(lock_);
        return outSampleRateHz != 0 && outInputChannels != 0 && outOutputChannels != 0;
    }

    [[nodiscard]] uint64_t CopyTopologyRevision() const noexcept {
        if (!lock_) return 0;
        IOLockLock(lock_);
        const uint64_t revision = topologyRevision_;
        IOLockUnlock(lock_);
        return revision;
    }

    // Update only the current sample rate. The DICE clock can change (Audio MIDI
    // Setup / Logic) without a full config rebuild; the next StartIO seeds the
    // direct-binding/ZTS clock from the live rate, so this must reflect it or
    // CoreAudio sees the device clock advancing at the wrong rate and churns
    // StartIO/StopIO.
    //
    // The binding the IR actually reads reports directSampleRateHz_, not
    // currentSampleRateHz_. That field (and directGeneration_) is latched
    // only when the direct-audio memory is allocated in
    // EnsureDirectAudioMemoryLocked, which does not re-run on an idle rate change
    // because the ring buffers are rate-independent (fixed kAudioRingBufferFrames)
    // and are allocated once at bring-up then reused. So updating the numeric
    // endpoint geometry alone
    // leaves the binding (and the ZTS the HAL judges) stuck at the publish-time
    // 48 kHz. Refresh directSampleRateHz_ and bump the generation here so
    // CopyDirectAudioBinding reports the new rate and the IR re-arms the RX/ZTS
    // clock at it.
    void SetCurrentSampleRate(uint32_t sampleRateHz) noexcept {
        if (sampleRateHz == 0) {
            return;
        }
        if (lock_) {
            IOLockLock(lock_);
        }
        const bool changed = currentSampleRateHz_ != sampleRateHz;
        currentSampleRateHz_ = sampleRateHz;
        if (changed) {
            ++topologyRevision_;
        }
        if (directSampleRateHz_ != 0 && directSampleRateHz_ != sampleRateHz) {
            directSampleRateHz_ = sampleRateHz;
            directActiveOutputRingFrames_ =
                Shared::AudioTimingGeometry::FrameRingFrames(sampleRateHz);
            directActiveInputRingFrames_ =
                Shared::AudioTimingGeometry::FrameRingFrames(sampleRateHz);
            ++directGeneration_;
        }
        if (lock_) {
            IOLockUnlock(lock_);
        }
    }

    // The descriptor allocation is fixed at the endpoint's immutable maximum
    // capability envelope. A coordinator commit changes only active geometry,
    // so neither CoreAudio nor the transport observes a replaced descriptor.
    [[nodiscard]] bool ApplyConfiguration(
        const AudioStreamRuntimeCaps& runtimeCaps) noexcept {
        const uint32_t outputChannels = PhysicalPlaybackChannels(runtimeCaps);
        const uint32_t inputChannels = PhysicalCaptureChannels(runtimeCaps);
        const uint32_t activeRingFrames =
            Shared::AudioTimingGeometry::FrameRingFrames(runtimeCaps.sampleRateHz);
        const uint64_t reqOutBytes =
            static_cast<uint64_t>(activeRingFrames) * outputChannels * sizeof(float);
        const uint64_t reqInBytes =
            static_cast<uint64_t>(activeRingFrames) * inputChannels * sizeof(float);
        if (runtimeCaps.sampleRateHz == 0 || outputChannels == 0 || inputChannels == 0 ||
            outputChannels > maximumOutputChannels_ ||
            inputChannels > maximumInputChannels_ ||
            (allocationLimits_.maxAllocatedFrames != 0 &&
             activeRingFrames > allocationLimits_.maxAllocatedFrames) ||
            (allocationLimits_.allocatedOutputBytes != 0 &&
             reqOutBytes > allocationLimits_.allocatedOutputBytes) ||
            (allocationLimits_.allocatedInputBytes != 0 &&
             reqInBytes > allocationLimits_.allocatedInputBytes) ||
            !lock_) {
            ASFW_LOG_ERROR(DirectAudio,
                           "[AudioConfig] runtime projection rejected guid=0x%016llx rate=%u out=%u/%u in=%u/%u frames=%u reqOut=%llu/%llu reqIn=%llu/%llu",
                           observedGuid_, runtimeCaps.sampleRateHz, outputChannels,
                           maximumOutputChannels_, inputChannels, maximumInputChannels_,
                           activeRingFrames, reqOutBytes, allocationLimits_.allocatedOutputBytes,
                           reqInBytes, allocationLimits_.allocatedInputBytes);
            return false;
        }
        IOLockLock(lock_);
        const bool changed = currentSampleRateHz_ != runtimeCaps.sampleRateHz ||
                             configuredOutputChannels_ != outputChannels ||
                             configuredInputChannels_ != inputChannels;
        currentSampleRateHz_ = runtimeCaps.sampleRateHz;
        configuredOutputChannels_ = outputChannels;
        configuredInputChannels_ = inputChannels;
        if (changed) {
            ++topologyRevision_;
        }
        if (HasCompleteDirectAudioMemoryLocked() && changed) {
            directActiveOutputRingFrames_ =
                Shared::AudioTimingGeometry::FrameRingFrames(runtimeCaps.sampleRateHz);
            directActiveInputRingFrames_ =
                Shared::AudioTimingGeometry::FrameRingFrames(runtimeCaps.sampleRateHz);
            directOutputChannels_ = outputChannels;
            directInputChannels_ = inputChannels;
            directSampleRateHz_ = runtimeCaps.sampleRateHz;
            ++directGeneration_;
            ASFW_LOG(DirectAudio,
                     "[AudioConfig] runtime projection committed guid=0x%016llx gen=%llu rate=%u out=%u/%u in=%u/%u",
                     observedGuid_, directGeneration_, directSampleRateHz_,
                     directOutputChannels_, directOutputCapacityChannels_,
                     directInputChannels_, directInputCapacityChannels_);
        }
        IOLockUnlock(lock_);
        return true;
    }

    [[nodiscard]] kern_return_t EnsureDirectAudioMemory() noexcept {
        if (!lock_) {
            return kIOReturnNotReady;
        }
        if (lock_) {
            IOLockLock(lock_);
        }
        const kern_return_t kr = EnsureDirectAudioMemoryLocked();
        if (lock_) {
            IOLockUnlock(lock_);
        }
        return kr;
    }

    [[nodiscard]] kern_return_t CopyDirectAudioMemory(IOMemoryDescriptor** outOutputMemory,
                                                      IOMemoryDescriptor** outInputMemory,
                                                      IOMemoryDescriptor** outControlMemory,
                                                      uint32_t* outOutputFrames,
                                                      uint32_t* outOutputChannels,
                                                      uint32_t* outInputFrames,
                                                      uint32_t* outInputChannels,
                                                      uint32_t* outSampleRateHz,
                                                      uint64_t* outGeneration) noexcept {
        if (outOutputMemory) { *outOutputMemory = nullptr; }
        if (outInputMemory) { *outInputMemory = nullptr; }
        if (outControlMemory) { *outControlMemory = nullptr; }
        if (outOutputFrames) { *outOutputFrames = 0; }
        if (outOutputChannels) { *outOutputChannels = 0; }
        if (outInputFrames) { *outInputFrames = 0; }
        if (outInputChannels) { *outInputChannels = 0; }
        if (outSampleRateHz) { *outSampleRateHz = 0; }
        if (outGeneration) { *outGeneration = 0; }

        if (!outOutputMemory || !outInputMemory || !outControlMemory ||
            !outOutputFrames || !outOutputChannels || !outInputFrames || !outInputChannels ||
            !outSampleRateHz || !outGeneration) {
            ASFW_LOG(DirectAudio,
                     "ADK DBG MEM runtime copy failed bad_args observedGuid=0x%016llx",
                     observedGuid_);
            return kIOReturnBadArgument;
        }

        if (!lock_) {
            ASFW_LOG(DirectAudio,
                     "ADK DBG MEM runtime copy failed no_lock observedGuid=0x%016llx",
                     observedGuid_);
            return kIOReturnNotReady;
        }

        if (lock_) {
            IOLockLock(lock_);
        }

        kern_return_t kr = EnsureDirectAudioMemoryLocked();
        if (kr != kIOReturnSuccess) {
            ASFW_LOG(DirectAudio,
                     "ADK DBG MEM runtime copy ensure_failed observedGuid=0x%016llx kr=0x%x",
                     observedGuid_,
                     kr);
            if (lock_) {
                IOLockUnlock(lock_);
            }
            return kr;
        }

        if (!HasCompleteDirectAudioMemoryLocked()) {
            ASFW_LOG(DirectAudio,
                     "ADK DBG MEM runtime copy failed incomplete observedGuid=0x%016llx gen=%llu outMem=%p inMem=%p ctlMem=%p outMap=%p inMap=%p ctlMap=%p control=%p rate=%u",
                     observedGuid_,
                     directGeneration_,
                     static_cast<void*>(directOutputMemory_),
                     static_cast<void*>(directInputMemory_),
                     static_cast<void*>(directControlMemory_),
                     static_cast<void*>(directOutputMap_),
                     static_cast<void*>(directInputMap_),
                     static_cast<void*>(directControlMap_),
                     static_cast<void*>(directControl_),
                     directSampleRateHz_);
            if (lock_) {
                IOLockUnlock(lock_);
            }
            return kIOReturnNotReady;
        }

        directOutputMemory_->retain();
        directInputMemory_->retain();
        directControlMemory_->retain();

        *outOutputMemory = directOutputMemory_;
        *outInputMemory = directInputMemory_;
        *outControlMemory = directControlMemory_;
        *outOutputFrames = directActiveOutputRingFrames_;
        *outOutputChannels = directOutputChannels_;
        *outInputFrames = directActiveInputRingFrames_;
        *outInputChannels = directInputChannels_;
        *outSampleRateHz = directSampleRateHz_;
        *outGeneration = directGeneration_;

        ASFW_LOG(DirectAudio,
                 "ADK DBG MEM runtime copy ok observedGuid=0x%016llx gen=%llu outMem=%p outFrames=%u outCh=%u inMem=%p inFrames=%u inCh=%u controlMem=%p rate=%u",
                 observedGuid_,
                 *outGeneration,
                 static_cast<void*>(*outOutputMemory),
                 *outOutputFrames,
                 *outOutputChannels,
                 static_cast<void*>(*outInputMemory),
                 *outInputFrames,
                 *outInputChannels,
                 static_cast<void*>(*outControlMemory),
                 *outSampleRateHz);

        if (lock_) {
            IOLockUnlock(lock_);
        }
        return kIOReturnSuccess;
    }

    void ReleaseDirectAudioMemory() noexcept {
        if (lock_) {
            IOLockLock(lock_);
        }
        ReleaseDirectAudioMemoryLocked();
        if (lock_) {
            IOLockUnlock(lock_);
        }
    }

    [[nodiscard]] bool HasCompleteDirectAudioMemory() noexcept {
        if (!lock_) {
            return false;
        }
        IOLockLock(lock_);
        const bool complete = HasCompleteDirectAudioMemoryLocked();
        IOLockUnlock(lock_);
        return complete;
    }

    [[nodiscard]] bool CopyDirectAudioBinding(Runtime::DirectAudioBindingSnapshot& out) noexcept override {
        out = {};
        if (!lock_) {
            return false;
        }

        IOLockLock(lock_);
        if (!HasCompleteDirectAudioMemoryLocked()) {
            IOLockUnlock(lock_);
            ASFW_LOG_RL(DirectAudio, "endpoint_bind/not_ready", 1000, OS_LOG_TYPE_DEFAULT,
                        "ADK DBG BIND runtime get refused not_ready observedGuid=0x%016llx gen=%llu control=%p rate=%u outBase=%p outFrames=%u outCh=%u inBase=%p inFrames=%u inCh=%u",
                        observedGuid_,
                        directGeneration_,
                        static_cast<void*>(directControl_),
                        directSampleRateHz_,
                        static_cast<const void*>(directOutputBase_),
                        directActiveOutputRingFrames_,
                        directOutputChannels_,
                        static_cast<void*>(directInputBase_),
                        directActiveInputRingFrames_,
                        directInputChannels_);
            return false;
        }

        out.endpointId = endpointId_;
        out.generation = directGeneration_;
        out.outputBase = directOutputBase_;
        out.outputBytes = directOutputBytes_;
        out.outputFrames = directActiveOutputRingFrames_;
        out.outputChannels = directOutputChannels_;
        out.inputBase = directInputBase_;
        out.inputBytes = directInputBytes_;
        out.inputFrames = directActiveInputRingFrames_;
        out.inputChannels = directInputChannels_;
        out.control = directControl_;
        out.sampleRateHz = directSampleRateHz_;
        out.valid = true;
        IOLockUnlock(lock_);
        return true;
    }

    void MarkStreaming(bool streaming) noexcept {
        streaming_.store(streaming, std::memory_order_release);
    }

    [[nodiscard]] bool IsStreaming() const noexcept {
        return streaming_.load(std::memory_order_acquire);
    }

    // This captures atomics while the endpoint still owns the mapping.  It is
    // intentionally a value snapshot, never a directControl_ escape hatch.
    [[nodiscard]] bool CopyAudioTelemetrySnapshot(
        Runtime::AudioTelemetryEndpointSnapshot& out) noexcept {
        out = {};
        if (!lock_) {
            return false;
        }

        IOLockLock(lock_);
        out.endpointId = endpointId_.value;
        out.deviceInstanceId = deviceInstanceId_.value;
        out.observedGuid = observedGuid_;
        out.endpointGeneration = directGeneration_;
        if (streaming_.load(std::memory_order_acquire)) {
            out.flags |= Runtime::kAudioTelemetryStreaming;
        }
        out.preparationLeadPackets =
            Shared::AudioTimingGeometry::kTxPreparationLeadPackets;
        out.hardwareFloorPackets =
            Shared::AudioTimingGeometry::kTxHardwareRingPackets;

        if (HasCompleteDirectAudioMemoryLocked()) {
            out.flags |= Runtime::kAudioTelemetryBindingReady;
            out.sampleRateHz = directSampleRateHz_;
            out.outputChannels = directOutputChannels_;
            out.inputChannels = directInputChannels_;
            out.inputFrameCapacityFrames = directActiveInputRingFrames_;
            Runtime::CopyAudioTelemetrySnapshot(*directControl_, out);
        } else {
            // Keep a registered endpoint visible to diagnostics even before a
            // complete mapping exists (or after one was lost). Previously it
            // disappeared from the registry snapshot, making MCP's
            // endpointCount=0 indistinguishable from decoder/user-client
            // failure. No pointer escapes; only stable config values are used.
            out.sampleRateHz = currentSampleRateHz_;
            out.outputChannels = configuredOutputChannels_;
            out.inputChannels = configuredInputChannels_;
        }
        IOLockUnlock(lock_);
        return true;
    }

    void RegisterTxLatencySession(std::shared_ptr<Runtime::TxLatencySession> session) noexcept {
        if (lock_) {
            IOLockLock(lock_);
            txLatencySession_ = std::move(session);
            IOLockUnlock(lock_);
        } else {
            txLatencySession_ = std::move(session);
        }
    }

    void UnregisterTxLatencySession(const std::shared_ptr<Runtime::TxLatencySession>& session) noexcept {
        std::shared_ptr<Runtime::TxLatencySession> oldSession;
        if (lock_) {
            IOLockLock(lock_);
            if (!session || txLatencySession_ == session) {
                oldSession = std::move(txLatencySession_);
                txLatencySession_.reset();
            }
            IOLockUnlock(lock_);
        } else {
            if (!session || txLatencySession_ == session) {
                oldSession = std::move(txLatencySession_);
                txLatencySession_.reset();
            }
        }
        if (oldSession) {
            oldSession->RequestStop(Runtime::TxLatencyTerminationReason::DriverTeardown);
            oldSession->PollQuiescence();
        }
    }

    [[nodiscard]] bool StartTxLatencySession(uint32_t durationSeconds,
                                             uint32_t strataSize,
                                             uint32_t seed,
                                             uint32_t assumedDriftPpm,
                                             uint32_t* outSessionId = nullptr) noexcept {
        std::shared_ptr<Runtime::TxLatencySession> session;
        uint64_t epoch = 0;
        uint32_t rate = currentSampleRateHz_;
        if (lock_) {
            IOLockLock(lock_);
            session = txLatencySession_;
            if (directControl_) {
                epoch = directControl_->hardwareTimeline.Epoch();
            }
            if (directSampleRateHz_ != 0) {
                rate = directSampleRateHz_;
            }
            IOLockUnlock(lock_);
        } else {
            session = txLatencySession_;
        }
        if (!session) return false;

        uint32_t sid = nextSessionId_.fetch_add(1, std::memory_order_relaxed);
        if (sid == 0) {
            sid = nextSessionId_.fetch_add(1, std::memory_order_relaxed);
        }
        if (outSessionId) {
            *outSessionId = sid;
        }

        return session->Arm(sid, epoch, rate, durationSeconds,
                            Runtime::kTxLatencyMaxSamples, seed, strataSize, assumedDriftPpm);
    }

    [[nodiscard]] bool StopTxLatencySession() noexcept {
        std::shared_ptr<Runtime::TxLatencySession> session;
        if (lock_) {
            IOLockLock(lock_);
            session = txLatencySession_;
            IOLockUnlock(lock_);
        } else {
            session = txLatencySession_;
        }
        if (!session) return false;
        session->RequestStop(Runtime::TxLatencyTerminationReason::UserStopped);
        session->PollQuiescence();
        return true;
    }

    [[nodiscard]] bool CopyTxLatencyResults(
        uint32_t pageIndex,
        uint32_t samplesPerPage,
        uint32_t requestedSessionId,
        UserClient::Wire::TxLatencyResultsPageWire& out) const noexcept {
        std::shared_ptr<Runtime::TxLatencySession> session;
        if (lock_) {
            IOLockLock(lock_);
            session = txLatencySession_;
            IOLockUnlock(lock_);
        } else {
            session = txLatencySession_;
        }
        if (!session) return false;
        return session->CopyWirePage(pageIndex, samplesPerPage, requestedSessionId, endpointId_.value, out);
    }

private:
    [[nodiscard]] static uint32_t SumPcmChannels(
        const AudioStreamWireInfo* streams, uint32_t count) noexcept {
        uint32_t total = 0;
        const uint32_t bounded = count < kMaxAudioStreamsPerDirection
                                     ? count
                                     : kMaxAudioStreamsPerDirection;
        for (uint32_t i = 0; i < bounded; ++i) {
            total += streams[i].pcmChannels;
        }
        return total;
    }

    [[nodiscard]] static uint32_t PhysicalPlaybackChannels(
        const AudioStreamRuntimeCaps& caps) noexcept {
        const uint32_t fromStreams = SumPcmChannels(
            caps.hostToDeviceStreams, caps.hostToDeviceStreamCount);
        return ClampAudioChannels(fromStreams != 0
                                      ? fromStreams
                                      : caps.hostOutputPcmChannels);
    }

    [[nodiscard]] static uint32_t PhysicalCaptureChannels(
        const AudioStreamRuntimeCaps& caps) noexcept {
        const uint32_t fromStreams = SumPcmChannels(
            caps.deviceToHostStreams, caps.deviceToHostStreamCount);
        return ClampAudioChannels(fromStreams != 0
                                      ? fromStreams
                                      : caps.hostInputPcmChannels);
    }

    [[nodiscard]] static uint32_t MaximumPlaybackChannels(
        const Devices::ResolvedAudioEndpointProfile& profile) noexcept {
        uint32_t maximum = PhysicalPlaybackChannels(profile.runtimeCaps);
        const uint8_t count = std::min(profile.configurationCapabilityCount,
                                       static_cast<uint8_t>(profile.configurationCapabilities.size()));
        for (uint8_t i = 0; i < count; ++i) {
            maximum = std::max(maximum, PhysicalPlaybackChannels(
                profile.configurationCapabilities[i].runtimeCaps));
        }
        return maximum;
    }

    [[nodiscard]] static uint32_t MaximumCaptureChannels(
        const Devices::ResolvedAudioEndpointProfile& profile) noexcept {
        uint32_t maximum = PhysicalCaptureChannels(profile.runtimeCaps);
        const uint8_t count = std::min(profile.configurationCapabilityCount,
                                       static_cast<uint8_t>(profile.configurationCapabilities.size()));
        for (uint8_t i = 0; i < count; ++i) {
            maximum = std::max(maximum, PhysicalCaptureChannels(
                profile.configurationCapabilities[i].runtimeCaps));
        }
        return maximum;
    }

    [[nodiscard]] static uint32_t ClampAudioChannels(uint32_t channels) noexcept {
        if (channels == 0) {
            return 0;
        }
        return (channels > ASFW::Encoding::kMaxPcmChannels)
            ? ASFW::Encoding::kMaxPcmChannels
            : channels;
    }

    [[nodiscard]] static kern_return_t CreateMappedDirectBuffer(uint64_t bytes,
                                                                uint64_t alignment,
                                                                IOBufferMemoryDescriptor** outMemory,
                                                                IOMemoryMap** outMap) noexcept {
        if (!outMemory || !outMap || bytes == 0) {
            return kIOReturnBadArgument;
        }

        *outMemory = nullptr;
        *outMap = nullptr;

        IOBufferMemoryDescriptor* memory = nullptr;
        kern_return_t kr = IOBufferMemoryDescriptor::Create(kIOMemoryDirectionInOut,
                                                            bytes,
                                                            alignment,
                                                            &memory);
        if (kr != kIOReturnSuccess || !memory) {
            return (kr == kIOReturnSuccess) ? kIOReturnNoMemory : kr;
        }

        IOMemoryMap* map = nullptr;
        kr = memory->CreateMapping(0, 0, 0, bytes, 0, &map);
        if (kr != kIOReturnSuccess || !map) {
            if (map) {
                map->release();
            }
            memory->release();
            return (kr == kIOReturnSuccess) ? kIOReturnNoMemory : kr;
        }

        *outMemory = memory;
        *outMap = map;
        return kIOReturnSuccess;
    }

    [[nodiscard]] bool HasCompleteDirectAudioMemoryLocked() const noexcept {
        return directOutputMemory_ &&
               directInputMemory_ &&
               directControlMemory_ &&
               directOutputMap_ &&
               directInputMap_ &&
               directControlMap_ &&
               directOutputBase_ &&
               directInputBase_ &&
               directControl_ &&
               directActiveOutputRingFrames_ != 0 &&
               directActiveInputRingFrames_ != 0 &&
               directOutputChannels_ != 0 &&
               directInputChannels_ != 0 &&
               directSampleRateHz_ != 0;
    }

    void PublishDirectAudioBindingFromMappedMemoryLocked() noexcept {
        if (!directOutputMap_ || !directInputMap_ || !directControlMap_) {
            return;
        }

        directOutputBase_ = reinterpret_cast<const float*>(
            static_cast<uintptr_t>(directOutputMap_->GetAddress()));
        directOutputBytes_ = directOutputMap_->GetLength();
        directInputBase_ = reinterpret_cast<float*>(
            static_cast<uintptr_t>(directInputMap_->GetAddress()));
        directInputBytes_ = directInputMap_->GetLength();
        directControl_ = reinterpret_cast<Runtime::AudioTransportControlBlock*>(
            static_cast<uintptr_t>(directControlMap_->GetAddress()));
        ++directGeneration_;

        ASFW_LOG(DirectAudio,
                 "ADK DBG BIND runtime local_memory_ready observedGuid=0x%016llx gen=%llu outBase=%p outBytes=%llu outFrames=%u outCh=%u inBase=%p inBytes=%llu inFrames=%u inCh=%u control=%p rate=%u",
                 observedGuid_,
                 directGeneration_,
                 static_cast<const void*>(directOutputBase_),
                 directOutputBytes_,
                 directActiveOutputRingFrames_,
                 directOutputChannels_,
                 static_cast<void*>(directInputBase_),
                 directInputBytes_,
                 directActiveInputRingFrames_,
                 directInputChannels_,
                 static_cast<void*>(directControl_),
                 directSampleRateHz_);
    }

    void ReleaseDirectAudioMemoryLocked() noexcept {
        directOutputBase_ = nullptr;
        directOutputBytes_ = 0;
        directActiveOutputRingFrames_ = 0;
        directOutputChannels_ = 0;
        directOutputCapacityChannels_ = 0;
        directInputBase_ = nullptr;
        directInputBytes_ = 0;
        directActiveInputRingFrames_ = 0;
        directInputChannels_ = 0;
        directInputCapacityChannels_ = 0;
        directControl_ = nullptr;
        directSampleRateHz_ = 0;
        ++directGeneration_;

        if (directOutputMap_) {
            directOutputMap_->release();
            directOutputMap_ = nullptr;
        }
        if (directInputMap_) {
            directInputMap_->release();
            directInputMap_ = nullptr;
        }
        if (directControlMap_) {
            directControlMap_->release();
            directControlMap_ = nullptr;
        }
        if (directOutputMemory_) {
            directOutputMemory_->release();
            directOutputMemory_ = nullptr;
        }
        if (directInputMemory_) {
            directInputMemory_->release();
            directInputMemory_ = nullptr;
        }
        if (directControlMemory_) {
            directControlMemory_->release();
            directControlMemory_ = nullptr;
        }
    }

    [[nodiscard]] kern_return_t EnsureDirectAudioMemoryLocked() noexcept {
        // Direct memory serves the physical DICE transport, not only visible
        // CoreAudio streams. A playback-only device can retain an operational
        // device->host stream for its firmware/clock protocol while explicitly
        // publishing zero CoreAudio input channels. In that case the aggregate
        // count is the transport buffer's safe fallback geometry.
        const uint32_t outputChannels = configuredOutputChannels_;
        const uint32_t inputChannels = configuredInputChannels_;
        const uint32_t outputCapacityChannels = maximumOutputChannels_;
        const uint32_t inputCapacityChannels = maximumInputChannels_;
        const uint32_t sampleRateHz = currentSampleRateHz_;
        const uint32_t activeFrames =
            Shared::AudioTimingGeometry::FrameRingFrames(sampleRateHz);
        const uint32_t outputFrames = activeFrames;
        const uint32_t inputFrames = activeFrames;

        if (outputChannels == 0 || inputChannels == 0 || outputCapacityChannels == 0 ||
            inputCapacityChannels == 0 || sampleRateHz == 0) {
            ASFW_LOG(DirectAudio,
                     "ADK DBG MEM runtime ensure failed bad_config observedGuid=0x%016llx agg=%u in=%u out=%u rate=%u",
                     observedGuid_,
                     std::max(inputChannels, outputChannels),
                     inputChannels,
                     outputChannels,
                     sampleRateHz);
            return kIOReturnBadArgument;
        }

        if (HasCompleteDirectAudioMemoryLocked()) {
            directActiveOutputRingFrames_ = outputFrames;
            directActiveInputRingFrames_ = inputFrames;
            directOutputChannels_ = outputChannels;
            directInputChannels_ = inputChannels;
            directSampleRateHz_ = sampleRateHz;
            ASFW_LOG(DirectAudio,
                     "ADK DBG MEM runtime ensure reuse observedGuid=0x%016llx gen=%llu outFrames=%u outCh=%u inFrames=%u inCh=%u rate=%u",
                     observedGuid_,
                     directGeneration_,
                     outputFrames,
                     directOutputChannels_,
                     inputFrames,
                     directInputChannels_,
                     sampleRateHz);
            return kIOReturnSuccess;
        }

        ReleaseDirectAudioMemoryLocked();

        const uint64_t outputBytes = allocatedOutputBytes_;
        const uint64_t inputBytes = allocatedInputBytes_;
        const uint64_t controlBytes = sizeof(Runtime::AudioTransportControlBlock);

        ASFW_LOG(DirectAudio,
                 "ADK DBG MEM runtime allocate observedGuid=0x%016llx outBytes=%llu outFrames=%u outCh=%u inBytes=%llu inFrames=%u inCh=%u controlBytes=%llu rate=%u",
                 observedGuid_,
                 outputBytes,
                 outputFrames,
                 outputChannels,
                 inputBytes,
                 inputFrames,
                 inputChannels,
                 controlBytes,
                 sampleRateHz);

        kern_return_t kr = CreateMappedDirectBuffer(outputBytes, 64, &directOutputMemory_, &directOutputMap_);
        if (kr != kIOReturnSuccess) {
            ASFW_LOG(DirectAudio, "ADK DBG MEM runtime allocate output failed observedGuid=0x%016llx kr=0x%x", observedGuid_, kr);
            ReleaseDirectAudioMemoryLocked();
            return kr;
        }
        kr = CreateMappedDirectBuffer(inputBytes, 64, &directInputMemory_, &directInputMap_);
        if (kr != kIOReturnSuccess) {
            ASFW_LOG(DirectAudio, "ADK DBG MEM runtime allocate input failed observedGuid=0x%016llx kr=0x%x", observedGuid_, kr);
            ReleaseDirectAudioMemoryLocked();
            return kr;
        }
        kr = CreateMappedDirectBuffer(controlBytes,
                                      alignof(Runtime::AudioTransportControlBlock),
                                      &directControlMemory_,
                                      &directControlMap_);
        if (kr != kIOReturnSuccess) {
            ASFW_LOG(DirectAudio, "ADK DBG MEM runtime allocate control failed observedGuid=0x%016llx kr=0x%x", observedGuid_, kr);
            ReleaseDirectAudioMemoryLocked();
            return kr;
        }

        directActiveOutputRingFrames_ = outputFrames;
        directOutputChannels_ = outputChannels;
        directOutputCapacityChannels_ = outputCapacityChannels;
        directActiveInputRingFrames_ = inputFrames;
        directInputChannels_ = inputChannels;
        directInputCapacityChannels_ = inputCapacityChannels;
        directSampleRateHz_ = sampleRateHz;

        std::memset(reinterpret_cast<void*>(static_cast<uintptr_t>(directOutputMap_->GetAddress())),
                    0,
                    static_cast<size_t>(directOutputMap_->GetLength()));
        std::memset(reinterpret_cast<void*>(static_cast<uintptr_t>(directInputMap_->GetAddress())),
                    0,
                    static_cast<size_t>(directInputMap_->GetLength()));
        auto* control = reinterpret_cast<Runtime::AudioTransportControlBlock*>(
            static_cast<uintptr_t>(directControlMap_->GetAddress()));
        new (control) Runtime::AudioTransportControlBlock();
        control->ResetForStart();

        PublishDirectAudioBindingFromMappedMemoryLocked();
        return HasCompleteDirectAudioMemoryLocked() ? kIOReturnSuccess : kIOReturnNotReady;
    }

    [[nodiscard]] static Shared::DirectAudioAllocationLimits
    ComputeAllocationLimits(
        const Devices::ResolvedAudioEndpointProfile& profile) noexcept {
        uint64_t maxOutBytes = 0;
        uint64_t maxInBytes = 0;
        const uint32_t maxOutCh = MaximumPlaybackChannels(profile);
        const uint32_t maxInCh = MaximumCaptureChannels(profile);

        const uint8_t count = std::min(
            profile.configurationCapabilityCount,
            static_cast<uint8_t>(profile.configurationCapabilities.size()));
        for (uint8_t i = 0; i < count; ++i) {
            const auto& caps = profile.configurationCapabilities[i].runtimeCaps;
            if (caps.sampleRateHz == 0 || caps.sampleRateHz > 96000) continue;
            const uint32_t frames =
                Shared::AudioTimingGeometry::FrameRingFrames(caps.sampleRateHz);
            const uint64_t outBytes =
                static_cast<uint64_t>(frames) * PhysicalPlaybackChannels(caps) * sizeof(float);
            const uint64_t inBytes =
                static_cast<uint64_t>(frames) * PhysicalCaptureChannels(caps) * sizeof(float);
            maxOutBytes = std::max(maxOutBytes, outBytes);
            maxInBytes = std::max(maxInBytes, inBytes);
        }

        const auto& fallbackCaps = profile.runtimeCaps;
        const uint32_t fallbackRate =
            fallbackCaps.sampleRateHz != 0 ? fallbackCaps.sampleRateHz : 48000;
        if (fallbackRate <= 96000) {
            const uint32_t frames =
                Shared::AudioTimingGeometry::FrameRingFrames(fallbackRate);
            const uint64_t outBytes =
                static_cast<uint64_t>(frames) * PhysicalPlaybackChannels(fallbackCaps) * sizeof(float);
            const uint64_t inBytes =
                static_cast<uint64_t>(frames) * PhysicalCaptureChannels(fallbackCaps) * sizeof(float);
            maxOutBytes = std::max(maxOutBytes, outBytes);
            maxInBytes = std::max(maxInBytes, inBytes);
        }

        if (count == 0) {
            maxOutBytes = std::max(
                maxOutBytes,
                static_cast<uint64_t>(Shared::AudioTimingGeometry::kAllocatedFrameRingFrames) *
                    maxOutCh * sizeof(float));
            maxInBytes = std::max(
                maxInBytes,
                static_cast<uint64_t>(Shared::AudioTimingGeometry::kAllocatedFrameRingFrames) *
                    maxInCh * sizeof(float));
        }

        if (maxOutBytes == 0 && maxOutCh != 0) {
            maxOutBytes = static_cast<uint64_t>(Shared::AudioTimingGeometry::kAllocatedFrameRingFrames) *
                maxOutCh * sizeof(float);
        }
        if (maxInBytes == 0 && maxInCh != 0) {
            maxInBytes = static_cast<uint64_t>(Shared::AudioTimingGeometry::kAllocatedFrameRingFrames) *
                maxInCh * sizeof(float);
        }

        if (maxOutBytes == 0) maxOutBytes = 64;
        if (maxInBytes == 0) maxInBytes = 64;

        return Shared::DirectAudioAllocationLimits{
            .allocatedOutputBytes = maxOutBytes,
            .allocatedInputBytes = maxInBytes,
            .maxOutputChannels = maxOutCh,
            .maxInputChannels = maxInCh,
            .maxAllocatedFrames = Shared::AudioTimingGeometry::kAllocatedFrameRingFrames,
        };
    }

    const Devices::AudioEndpointId endpointId_{};
    const Discovery::DeviceInstanceId deviceInstanceId_{};
    const uint64_t observedGuid_{0}; // Config-ROM evidence; diagnostics only
    const uint32_t maximumOutputChannels_{0};
    const uint32_t maximumInputChannels_{0};
    uint32_t configuredOutputChannels_{0};
    uint32_t configuredInputChannels_{0};
    uint32_t currentSampleRateHz_{0};
    const Shared::DirectAudioAllocationLimits allocationLimits_{};
    const uint64_t allocatedOutputBytes_{0};
    const uint64_t allocatedInputBytes_{0};
    /// Structural configuration revision. Control and meter frames must name
    /// this exact resolved stream/topology shape before the UI combines them.
    uint64_t topologyRevision_{1};
    mutable IOLock* lock_{nullptr};
    std::atomic<bool> streaming_{false};

    uint64_t directGeneration_{0};
    IOBufferMemoryDescriptor* directOutputMemory_{nullptr};
    IOBufferMemoryDescriptor* directInputMemory_{nullptr};
    IOBufferMemoryDescriptor* directControlMemory_{nullptr};
    IOMemoryMap* directOutputMap_{nullptr};
    IOMemoryMap* directInputMap_{nullptr};
    IOMemoryMap* directControlMap_{nullptr};
    const float* directOutputBase_{nullptr};
    uint64_t directOutputBytes_{0};
    uint32_t directActiveOutputRingFrames_{0};
    uint32_t directOutputChannels_{0};
    uint32_t directOutputCapacityChannels_{0};
    float* directInputBase_{nullptr};
    uint64_t directInputBytes_{0};
    uint32_t directActiveInputRingFrames_{0};
    uint32_t directInputChannels_{0};
    uint32_t directInputCapacityChannels_{0};
    Runtime::AudioTransportControlBlock* directControl_{nullptr};
    uint32_t directSampleRateHz_{0};
    std::shared_ptr<Runtime::TxLatencySession> txLatencySession_{nullptr};
    mutable std::atomic<uint32_t> nextSessionId_{1};
};

} // namespace ASFW::Audio
