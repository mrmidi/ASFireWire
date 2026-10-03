// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project

#pragma once

#include "../DriverKit/Runtime/DirectAudioBindingSource.hpp"
#include "../Runtime/AudioTelemetrySnapshot.hpp"
#include "../Model/ASFWAudioDevice.hpp"
#include "../Config/AudioConstants.hpp"
#include "../Wire/AMDTP/AmdtpRateGeometry.hpp"
#include "../../Logging/Logging.hpp"
#include "../../Shared/ASFWAudioStreamMetricsABI.h"
#include "../../Shared/ASFWIsochOracleCaptureABI.h"

#include <DriverKit/IOLib.h>

#if defined(ASFW_HOST_TEST)
#include "../../Testing/HostDriverKitStubs.hpp"
#else
#include <DriverKit/IOBufferMemoryDescriptor.h>
#include <DriverKit/IOMemoryDescriptor.h>
#include <DriverKit/IOMemoryMap.h>
#endif

#include <atomic>
#include <cstring>
#include <cstdint>
#include <new>

namespace ASFW::Audio {

// The oracle chunk is filled by copying runtime records verbatim, so the two
// layouts must not drift apart silently.
static_assert(sizeof(ASFWIsochOracleRecordV1) ==
                  sizeof(Runtime::IsochOracleRecord),
              "oracle export record must mirror the runtime record layout");
static_assert(ASFW_ISOCH_ORACLE_CAPTURE_CHUNK_RECORDS <=
                  Runtime::kIsochOracleCaptureRecords,
              "a chunk must never claim more records than a section can hold");
struct AudioOutputObserverState final {
    uint64_t writeEndFrame{0};
    uint64_t oldestValidFrame{0};
    uint64_t sessionEpoch{0};
    uint64_t discontinuityEpoch{0};
    uint64_t memoryGeneration{0};
    uint32_t activeRingFrames{0};
    uint32_t channels{0};
    uint32_t sampleRateHz{0};
};

class AudioEndpointRuntime final : public Runtime::IDirectAudioBindingSource {
public:
    explicit AudioEndpointRuntime(uint64_t guid) noexcept : guid_(guid), lock_(IOLockAlloc()) {}
    ~AudioEndpointRuntime() noexcept {
        ReleaseDirectAudioMemory();
        if (lock_) {
            IOLockFree(lock_);
            lock_ = nullptr;
        }
    }

    AudioEndpointRuntime(const AudioEndpointRuntime&) = delete;
    AudioEndpointRuntime& operator=(const AudioEndpointRuntime&) = delete;

    [[nodiscard]] uint64_t Guid() const noexcept { return guid_; }

    void UpdateConfig(const Model::ASFWAudioDevice& config) noexcept {
        if (lock_) {
            IOLockLock(lock_);
        }
        config_ = config;
        if (lock_) {
            IOLockUnlock(lock_);
        }
        configValid_.store(true, std::memory_order_release);
    }

    // Update only the current sample rate. The DICE clock can change (Audio MIDI
    // Setup / Logic) without a full config rebuild; the next StartIO seeds the
    // direct-binding/ZTS clock from the live rate, so this must reflect it or
    // CoreAudio sees the device clock advancing at the wrong rate and churns
    // StartIO/StopIO.
    //
    // The binding the IR actually reads reports directSampleRateHz_, not
    // config_.currentSampleRate. That field (and directGeneration_) is latched
    // only when the direct-audio memory is allocated in
    // EnsureDirectAudioMemoryLocked, which does not re-run on an idle rate change
    // because the ring buffers are allocated once at bring-up at the maximum
    // (kAudioRingBufferFrames) and then reused. So updating config_ alone
    // leaves the binding (and the ZTS the HAL judges) stuck at the publish-time
    // 48 kHz. Refresh directSampleRateHz_ and bump the generation here so
    // CopyDirectAudioBinding reports the new rate and the IR re-arms the RX/ZTS
    // clock at it. The ACTIVE ring moves with the rate (V3: 12288 frames at 1x,
    // 24576 at 2x) inside the unchanged allocation; the audio driver applies the
    // same HalBufferProfileForRate value to its graph binding in the rate-change
    // window, so both sides of the seam wrap on the same ring.
    void SetCurrentSampleRate(uint32_t sampleRateHz) noexcept {
        if (sampleRateHz == 0) {
            return;
        }
        if (lock_) {
            IOLockLock(lock_);
        }
        config_.currentSampleRate = sampleRateHz;
        const uint32_t activeFrames = ActiveRingFramesForRate(sampleRateHz);
        if (directSampleRateHz_ != 0 && directSampleRateHz_ != sampleRateHz &&
            activeFrames != 0) {
            directSampleRateHz_ = sampleRateHz;
            directOutputCapacityFrames_ = activeFrames;
            directInputCapacityFrames_ = activeFrames;
            ++directGeneration_;
        }
        if (lock_) {
            IOLockUnlock(lock_);
        }
    }

    [[nodiscard]] bool CopyConfig(Model::ASFWAudioDevice& outConfig) const noexcept {
        if (!configValid_.load(std::memory_order_acquire)) {
            return false;
        }
        if (lock_) {
            IOLockLock(lock_);
        }
        outConfig = config_;
        if (lock_) {
            IOLockUnlock(lock_);
        }
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
                     "ADK DBG MEM runtime copy failed bad_args guid=0x%016llx",
                     guid_);
            return kIOReturnBadArgument;
        }

        if (!lock_) {
            ASFW_LOG(DirectAudio,
                     "ADK DBG MEM runtime copy failed no_lock guid=0x%016llx",
                     guid_);
            return kIOReturnNotReady;
        }

        if (lock_) {
            IOLockLock(lock_);
        }

        kern_return_t kr = EnsureDirectAudioMemoryLocked();
        if (kr != kIOReturnSuccess) {
            ASFW_LOG(DirectAudio,
                     "ADK DBG MEM runtime copy ensure_failed guid=0x%016llx kr=0x%x",
                     guid_,
                     kr);
            if (lock_) {
                IOLockUnlock(lock_);
            }
            return kr;
        }

        if (!HasCompleteDirectAudioMemoryLocked()) {
            ASFW_LOG(DirectAudio,
                     "ADK DBG MEM runtime copy failed incomplete guid=0x%016llx gen=%llu outMem=%p inMem=%p ctlMem=%p outMap=%p inMap=%p ctlMap=%p control=%p rate=%u",
                     guid_,
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
        *outOutputFrames = directOutputCapacityFrames_;
        *outOutputChannels = directOutputChannels_;
        *outInputFrames = directInputCapacityFrames_;
        *outInputChannels = directInputChannels_;
        *outSampleRateHz = directSampleRateHz_;
        *outGeneration = directGeneration_;

        ASFW_LOG(DirectAudio,
                 "ADK DBG MEM runtime copy ok guid=0x%016llx gen=%llu outMem=%p outFrames=%u outCh=%u inMem=%p inFrames=%u inCh=%u controlMem=%p rate=%u",
                 guid_,
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

    // Diagnostic observer export. It retains the exact output descriptor used
    // by the AudioDriverKit stream and snapshots metadata under the runtime
    // lock; sample bytes remain shared and are never copied.
    [[nodiscard]] kern_return_t CopyOutputObserverMemory(
        IOMemoryDescriptor** outMemory,
        AudioOutputObserverState& outState) noexcept {
        outState = {};
        if (outMemory) { *outMemory = nullptr; }
        if (!outMemory || !lock_) {
            return kIOReturnBadArgument;
        }
        IOLockLock(lock_);
        if (!HasCompleteDirectAudioMemoryLocked() || !directControl_) {
            IOLockUnlock(lock_);
            return kIOReturnNotReady;
        }
        directOutputMemory_->retain();
        *outMemory = directOutputMemory_;
        CopyOutputObserverStateLocked(outState);
        IOLockUnlock(lock_);
        return kIOReturnSuccess;
    }

    [[nodiscard]] kern_return_t CopyOutputObserverState(
        AudioOutputObserverState& outState) noexcept {
        outState = {};
        if (!lock_) {
            return kIOReturnNotReady;
        }
        IOLockLock(lock_);
        if (!HasCompleteDirectAudioMemoryLocked() || !directControl_) {
            IOLockUnlock(lock_);
            return kIOReturnNotReady;
        }
        CopyOutputObserverStateLocked(outState);
        IOLockUnlock(lock_);
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
                        "ADK DBG BIND runtime get refused not_ready guid=0x%016llx gen=%llu control=%p rate=%u outBase=%p outFrames=%u outCh=%u inBase=%p inFrames=%u inCh=%u",
                        guid_,
                        directGeneration_,
                        static_cast<void*>(directControl_),
                        directSampleRateHz_,
                        static_cast<const void*>(directOutputBase_),
                        directOutputCapacityFrames_,
                        directOutputChannels_,
                        static_cast<void*>(directInputBase_),
                        directInputCapacityFrames_,
                        directInputChannels_);
            return false;
        }

        out.generation = directGeneration_;
        out.outputBase = directOutputBase_;
        out.outputBytes = directOutputBytes_;
        out.outputFrames = directOutputCapacityFrames_;
        out.outputChannels = directOutputChannels_;
        out.inputBase = directInputBase_;
        out.inputBytes = directInputBytes_;
        out.inputFrames = directInputCapacityFrames_;
        out.inputChannels = directInputChannels_;
        out.control = directControl_;
        out.sampleRateHz = directSampleRateHz_;
        out.valid = true;
        IOLockUnlock(lock_);
        return true;
    }

    void CopyAudioStreamMetricsSnapshot(
        ASFWAudioStreamMetricsSnapshotV1& out) const noexcept {
        out = {};
        out.abiVersion = ASFW_AUDIO_STREAM_METRICS_ABI_VERSION;
        out.structSize = sizeof(out);
        out.status = ASFWAudioStreamMetricsStatusUnavailable;
        out.guid = guid_;

        if (streaming_.load(std::memory_order_acquire)) {
            out.stateFlags |= ASFWAudioStreamMetricsStateStreaming;
        }
        if (!lock_) {
            return;
        }

        IOLockLock(lock_);
        if (configValid_.load(std::memory_order_acquire)) {
            out.stateFlags |= ASFWAudioStreamMetricsStateConfigAvailable;
            out.sampleRateHz = config_.currentSampleRate;
            out.outputChannels = config_.outputChannelCount
                ? config_.outputChannelCount
                : config_.channelCount;
            out.inputChannels = config_.inputChannelCount
                ? config_.inputChannelCount
                : config_.channelCount;
        }

        out.endpointGeneration = directGeneration_;
        if (!HasCompleteDirectAudioMemoryLocked()) {
            IOLockUnlock(lock_);
            return;
        }

        out.stateFlags |= ASFWAudioStreamMetricsStateControlAvailable;
        out.sampleRateHz = directSampleRateHz_;
        out.outputChannels = directOutputChannels_;
        out.inputChannels = directInputChannels_;

        const Runtime::AudioTransportControlBlock* control = directControl_;
        bool consistent = false;
        for (uint32_t attempt = 0; attempt < 4; ++attempt) {
            const uint64_t sequenceBefore =
                control->metricsSnapshotSequence.load(std::memory_order_acquire);
            if ((sequenceBefore & 1u) != 0) {
                continue;
            }
            const uint64_t generationBefore =
                control->generation.load(std::memory_order_acquire);

            out.streamGeneration = generationBefore;
            out.ioCallbackGeneration =
                control->ioCallbackGeneration.load(std::memory_order_relaxed);
            out.ioCallbackErrorGeneration =
                control->ioCallbackErrorGeneration.load(std::memory_order_relaxed);
            out.fatalGeneration =
                control->fatalGeneration.load(std::memory_order_relaxed);
            out.discontinuities =
                control->discontinuities.load(std::memory_order_relaxed);
            out.ioLastError = control->ioLastError.load(std::memory_order_relaxed);
            out.fatalReason = static_cast<uint32_t>(
                control->fatalReason.load(std::memory_order_relaxed));

            out.outputClientWriteEndFrame =
                control->client.outputClientWriteEndFrame.load(std::memory_order_relaxed);
            out.outputConsumedEndFrame =
                control->outputConsumedEndFrame.load(std::memory_order_relaxed);
            out.outputUnderruns =
                control->outputUnderruns.load(std::memory_order_relaxed);
            out.playbackRingWriteFrame =
                control->playbackRingWriteFrame.load(std::memory_order_relaxed);
            out.playbackRingReadFrame =
                control->playbackRingReadFrame.load(std::memory_order_relaxed);
            out.playbackRingOldestValidFrame =
                control->playbackRingOldestValidFrame.load(std::memory_order_relaxed);
            out.playbackRingUnderruns =
                control->playbackRingUnderruns.load(std::memory_order_relaxed);
            out.playbackRingOverruns =
                control->playbackRingOverruns.load(std::memory_order_relaxed);
            out.txPackets =
                control->counters.txPackets.load(std::memory_order_relaxed);
            out.txDataPackets =
                control->counters.txDataPackets.load(std::memory_order_relaxed);
            out.txNoDataPackets =
                control->counters.txNoDataPackets.load(std::memory_order_relaxed);
            out.txSilenceSubstitutions =
                control->counters.txSilenceSubstitutions.load(std::memory_order_relaxed);
            out.txPcmFramesEncoded =
                control->counters.txPcmFramesEncoded.load(std::memory_order_relaxed);
            out.txPcmNonzeroPackets =
                control->counters.txPcmNonzeroPackets.load(std::memory_order_relaxed);
            out.txPcmAllZeroPackets =
                control->counters.txPcmAllZeroPackets.load(std::memory_order_relaxed);
            // txScheduledSampleFrame, txCompletedSampleFrame and rxDbcFrameCount
            // left the control block as dead surfaces (de3fe1c7, T2).
            // Their snapshot fields stay in the ABI and read as zero.
            out.txScheduledSampleFrame = 0;
            out.txCompletedSampleFrame = 0;
            out.txReplayEntries =
                control->txReplayEntries.load(std::memory_order_relaxed);
            out.txReplayUnderflows =
                control->txReplayUnderflows.load(std::memory_order_relaxed);
            out.txReplayInvalidSyt =
                control->txReplayInvalidSyt.load(std::memory_order_relaxed);

            out.inputClientReadEndFrame =
                control->client.inputClientReadEndFrame.load(std::memory_order_relaxed);
            out.inputProducedEndFrame =
                control->inputProducedEndFrame.load(std::memory_order_relaxed);
            out.inputOverruns =
                control->inputOverruns.load(std::memory_order_relaxed);
            out.rxDbcFrameCount = 0;
            out.captureRingWriteFrame =
                control->captureRingWriteFrame.load(std::memory_order_relaxed);
            out.captureRingReadFrame =
                control->captureRingReadFrame.load(std::memory_order_relaxed);
            out.captureRingOverruns =
                control->captureRingOverruns.load(std::memory_order_relaxed);
            out.captureRingStarvations =
                control->captureRingStarvations.load(std::memory_order_relaxed);
            out.rxPackets =
                control->counters.rxPackets.load(std::memory_order_relaxed);
            out.rxDecodedFrames =
                control->counters.rxDecodedFrames.load(std::memory_order_relaxed);
            out.rxDiscontinuities =
                control->counters.rxDiscontinuities.load(std::memory_order_relaxed);
            out.rxReplayEntries =
                control->rxReplayEntries.load(std::memory_order_relaxed);
            out.rxReplayEpochResets =
                control->rxReplayEpochResets.load(std::memory_order_relaxed);
            out.ztsRxAdkPublished =
                control->counters.ztsRxAdkPublished.load(std::memory_order_relaxed);

            out.txMinimumPreparationDistance =
                control->txMinimumPreparationDistance.load(std::memory_order_relaxed);
            out.txMinimumCommittedMarginPackets =
                control->txMinimumCommittedMarginPackets.load(std::memory_order_relaxed);
            out.rxTransferDelayTicks =
                control->rxTransferDelayTicks.load(std::memory_order_relaxed);
            out.txTransferDelayTicks =
                control->txTransferDelayTicks.load(std::memory_order_relaxed);

            std::atomic_thread_fence(std::memory_order_acquire);
            const uint64_t generationAfter =
                control->generation.load(std::memory_order_relaxed);
            const uint64_t sequenceAfter =
                control->metricsSnapshotSequence.load(std::memory_order_relaxed);
            if (sequenceBefore == sequenceAfter &&
                (sequenceAfter & 1u) == 0 &&
                generationBefore == generationAfter) {
                consistent = true;
                break;
            }
        }

        if (consistent) {
            out.stateFlags |= ASFWAudioStreamMetricsStateConsistent;
            out.status = ASFWAudioStreamMetricsStatusOK;
        } else {
            out.status = ASFWAudioStreamMetricsStatusBusy;
        }
        IOLockUnlock(lock_);
    }

    // Reads one chunk of the bounded start-window oracle capture. Unlike the
    // metrics snapshot there is no consistency sequence to honour: the capture
    // is append-only within a generation, and the chunk reports that
    // generation so a reader that straddles a restart can discard the mix
    // rather than silently splice two starts together.
    void CopyIsochOracleCaptureChunk(
        ASFWIsochOracleCaptureChunkV1& out,
        uint32_t direction,
        uint32_t startIndex) const noexcept {
        const uint64_t guid = guid_;
        out = {};
        out.abiVersion = ASFW_ISOCH_ORACLE_CAPTURE_ABI_VERSION;
        out.structSize = sizeof(out);
        out.status = ASFWIsochOracleCaptureStatusUnavailable;
        out.guid = guid;
        out.direction = direction;
        out.startIndex = startIndex;
        out.sectionCapacity = Runtime::kIsochOracleCaptureRecords;

        if (direction != ASFWIsochOracleCaptureDirectionTx &&
            direction != ASFWIsochOracleCaptureDirectionRx) {
            out.status = ASFWIsochOracleCaptureStatusBadDirection;
            return;
        }
        if (!lock_) {
            return;
        }

        IOLockLock(lock_);
        if (!HasCompleteDirectAudioMemoryLocked() || !directControl_) {
            IOLockUnlock(lock_);
            return;
        }

        const auto& capture = directControl_->isochOracleCapture;
        const auto& section = capture.Section(
            direction == ASFWIsochOracleCaptureDirectionTx
                ? Runtime::IsochOracleDirection::kTx
                : Runtime::IsochOracleDirection::kRx);

        out.captureGeneration = capture.Generation();
        out.sectionCount = section.Count();
        out.frozen = section.Frozen() ? 1u : 0u;
        out.suppressed = section.Suppressed();
        out.lostCycles = section.LostCycles();
        out.backfilled = section.Backfilled();

        if (startIndex > out.sectionCount) {
            out.status = ASFWIsochOracleCaptureStatusOutOfRange;
            IOLockUnlock(lock_);
            return;
        }

        uint32_t emitted = 0;
        while (emitted < ASFW_ISOCH_ORACLE_CAPTURE_CHUNK_RECORDS) {
            Runtime::IsochOracleRecord record{};
            if (!section.ReadRecord(startIndex + emitted, record)) {
                break;
            }
            std::memcpy(&out.records[emitted], &record, sizeof(record));
            ++emitted;
        }
        out.recordCount = emitted;
        out.status = ASFWIsochOracleCaptureStatusOK;
        IOLockUnlock(lock_);
    }

    [[nodiscard]] bool IsCurrentStreamingRxEpoch(uint64_t epoch) noexcept {
        if (!lock_) return false;
        IOLockLock(lock_);
        const bool current = streaming_.load(std::memory_order_acquire) && directControl_ &&
            directControl_->rxReplayEpochResets.load(std::memory_order_acquire) == epoch;
        IOLockUnlock(lock_);
        return current;
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
        if (!HasCompleteDirectAudioMemoryLocked()) {
            IOLockUnlock(lock_);
            return false;
        }
        out.guid = guid_;
        out.endpointGeneration = directGeneration_;
        out.flags = Runtime::kAudioTelemetryBindingReady;
        if (streaming_.load(std::memory_order_acquire)) {
            out.flags |= Runtime::kAudioTelemetryStreaming;
        }
        out.sampleRateHz = directSampleRateHz_;
        out.outputChannels = directOutputChannels_;
        out.inputChannels = directInputChannels_;
        out.inputFrameCapacityFrames = directInputCapacityFrames_;
        out.preparationLeadPackets =
            IsochTransport::AudioTimingGeometry::kTxPreparationLeadPackets;
        out.hardwareFloorPackets =
            IsochTransport::AudioTimingGeometry::kTxHardwareRingPackets;
        Runtime::CopyAudioTelemetrySnapshot(*directControl_, out);
        IOLockUnlock(lock_);
        return true;
    }

private:
    void CopyOutputObserverStateLocked(AudioOutputObserverState& out) const noexcept {
        const auto& control = *directControl_;
        out.writeEndFrame =
            control.playbackRingWriteFrame.load(std::memory_order_acquire);
        out.oldestValidFrame =
            control.playbackRingOldestValidFrame.load(std::memory_order_relaxed);
        out.sessionEpoch = control.generation.load(std::memory_order_acquire);
        out.discontinuityEpoch =
            control.playbackRingDiscontinuityGeneration.load(std::memory_order_relaxed);
        out.memoryGeneration = directGeneration_;
        out.activeRingFrames = directOutputCapacityFrames_;
        out.channels = directOutputChannels_;
        out.sampleRateHz = directSampleRateHz_;
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
               directOutputCapacityFrames_ != 0 &&
               directInputCapacityFrames_ != 0 &&
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
                 "ADK DBG BIND runtime local_memory_ready guid=0x%016llx gen=%llu outBase=%p outBytes=%llu outFrames=%u outCh=%u inBase=%p inBytes=%llu inFrames=%u inCh=%u control=%p rate=%u",
                 guid_,
                 directGeneration_,
                 static_cast<const void*>(directOutputBase_),
                 directOutputBytes_,
                 directOutputCapacityFrames_,
                 directOutputChannels_,
                 static_cast<void*>(directInputBase_),
                 directInputBytes_,
                 directInputCapacityFrames_,
                 directInputChannels_,
                 static_cast<void*>(directControl_),
                 directSampleRateHz_);
    }

    void ReleaseDirectAudioMemoryLocked() noexcept {
        directOutputBase_ = nullptr;
        directOutputBytes_ = 0;
        directOutputCapacityFrames_ = 0;
        directOutputChannels_ = 0;
        directInputBase_ = nullptr;
        directInputBytes_ = 0;
        directInputCapacityFrames_ = 0;
        directInputChannels_ = 0;
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
        if (!configValid_.load(std::memory_order_acquire)) {
            ASFW_LOG(DirectAudio,
                     "ADK DBG MEM runtime ensure failed no_config guid=0x%016llx",
                     guid_);
            return kIOReturnNotReady;
        }

        // Direct memory serves the physical DICE transport, not only visible
        // CoreAudio streams. A playback-only device can retain an operational
        // device->host stream for its firmware/clock protocol while explicitly
        // publishing zero CoreAudio input channels. In that case the aggregate
        // count is the transport buffer's safe fallback geometry.
        const uint32_t outputChannels = ClampAudioChannels(
            config_.outputChannelCount ? config_.outputChannelCount : config_.channelCount);
        const uint32_t inputChannels = ClampAudioChannels(
            config_.inputChannelCount ? config_.inputChannelCount : config_.channelCount);
        const uint32_t sampleRateHz = config_.currentSampleRate ? config_.currentSampleRate : 48000;
        // Allocate the maximum once; publish the active ring for this rate.
        const uint32_t allocatedFrames = Isoch::Config::kAudioRingBufferFrames;
        const uint32_t outputFrames = ActiveRingFramesForRate(sampleRateHz);
        const uint32_t inputFrames = outputFrames;

        if (outputChannels == 0 || inputChannels == 0 || sampleRateHz == 0 ||
            outputFrames == 0) {
            ASFW_LOG(DirectAudio,
                     "ADK DBG MEM runtime ensure failed bad_config guid=0x%016llx agg=%u in=%u out=%u rate=%u",
                     guid_,
                     config_.channelCount,
                     inputChannels,
                     outputChannels,
                     sampleRateHz);
            return kIOReturnBadArgument;
        }

        if (HasCompleteDirectAudioMemoryLocked() &&
            directOutputCapacityFrames_ == outputFrames &&
            directInputCapacityFrames_ == inputFrames &&
            directOutputChannels_ == outputChannels &&
            directInputChannels_ == inputChannels &&
            directSampleRateHz_ == sampleRateHz) {
            ASFW_LOG(DirectAudio,
                     "ADK DBG MEM runtime ensure reuse guid=0x%016llx gen=%llu outFrames=%u outCh=%u inFrames=%u inCh=%u rate=%u",
                     guid_,
                     directGeneration_,
                     outputFrames,
                     outputChannels,
                     inputFrames,
                     inputChannels,
                     sampleRateHz);
            return kIOReturnSuccess;
        }

        ReleaseDirectAudioMemoryLocked();

        const uint64_t outputBytes = static_cast<uint64_t>(allocatedFrames) * outputChannels * sizeof(float);
        const uint64_t inputBytes = static_cast<uint64_t>(allocatedFrames) * inputChannels * sizeof(int32_t);
        const uint64_t controlBytes = sizeof(Runtime::AudioTransportControlBlock);

        ASFW_LOG(DirectAudio,
                 "ADK DBG MEM runtime allocate guid=0x%016llx outBytes=%llu outFrames=%u outCh=%u inBytes=%llu inFrames=%u inCh=%u controlBytes=%llu rate=%u",
                 guid_,
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
            ASFW_LOG(DirectAudio, "ADK DBG MEM runtime allocate output failed guid=0x%016llx kr=0x%x", guid_, kr);
            ReleaseDirectAudioMemoryLocked();
            return kr;
        }
        kr = CreateMappedDirectBuffer(inputBytes, 64, &directInputMemory_, &directInputMap_);
        if (kr != kIOReturnSuccess) {
            ASFW_LOG(DirectAudio, "ADK DBG MEM runtime allocate input failed guid=0x%016llx kr=0x%x", guid_, kr);
            ReleaseDirectAudioMemoryLocked();
            return kr;
        }
        kr = CreateMappedDirectBuffer(controlBytes,
                                      alignof(Runtime::AudioTransportControlBlock),
                                      &directControlMemory_,
                                      &directControlMap_);
        if (kr != kIOReturnSuccess) {
            ASFW_LOG(DirectAudio, "ADK DBG MEM runtime allocate control failed guid=0x%016llx kr=0x%x", guid_, kr);
            ReleaseDirectAudioMemoryLocked();
            return kr;
        }

        directOutputCapacityFrames_ = outputFrames;
        directOutputChannels_ = outputChannels;
        directInputCapacityFrames_ = inputFrames;
        directInputChannels_ = inputChannels;
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

    // Active ring at a rate: the HAL profile's ring when it fits the fixed
    // allocation, else 0 (the rate is not supported by this allocation).
    [[nodiscard]] static constexpr uint32_t ActiveRingFramesForRate(uint32_t sampleRateHz) noexcept {
        const auto hal = IsochTransport::HalBufferProfileForRate(sampleRateHz);
        return IsochTransport::IsValidAudioHalBufferProfile(hal) &&
                       IsochTransport::ProfileFitsAllocation(hal)
                   ? hal.frameRingFrames
                   : 0;
    }

    uint64_t guid_{0};
    mutable IOLock* lock_{nullptr};
    Model::ASFWAudioDevice config_{};
    std::atomic<bool> configValid_{false};
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
    uint32_t directOutputCapacityFrames_{0};
    uint32_t directOutputChannels_{0};
    float* directInputBase_{nullptr};
    uint64_t directInputBytes_{0};
    uint32_t directInputCapacityFrames_{0};
    uint32_t directInputChannels_{0};
    Runtime::AudioTransportControlBlock* directControl_{nullptr};
    uint32_t directSampleRateHz_{0};
};

} // namespace ASFW::Audio
