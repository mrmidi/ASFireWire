#include "TxPcmStagingRing.hpp"

#include <bit>
#include <limits>
#include <new>

namespace ASFW::Audio::Runtime {
namespace {
constexpr uint64_t kInvalidOrBusyFrameTag = UINT64_MAX;
}

bool TxPcmStagingRing::Configure(uint32_t channels,
                                 uint32_t frameCapacity) noexcept {
    if (channels == 0 || frameCapacity == 0) {
        return false;
    }
    const uint64_t sampleCount =
        static_cast<uint64_t>(channels) * frameCapacity;
    if (sampleCount > std::numeric_limits<size_t>::max() /
                          sizeof(std::atomic<uint32_t>)) {
        return false;
    }

    if (!sampleBits_ || channels_ != channels ||
        frameCapacity_ != frameCapacity) {
        auto sampleReplacement =
            std::unique_ptr<std::atomic<uint32_t>[]>(
                new (std::nothrow) std::atomic<uint32_t>[sampleCount]);
        auto tagReplacement =
            std::unique_ptr<std::atomic<uint64_t>[]>{
                new (std::nothrow) std::atomic<uint64_t>[frameCapacity]};
        if (!sampleReplacement || !tagReplacement) {
            return false;
        }
        for (uint64_t index = 0; index < sampleCount; ++index) {
            sampleReplacement[index].store(0, std::memory_order_relaxed);
        }
        for (uint32_t frame = 0; frame < frameCapacity; ++frame) {
            tagReplacement[frame].store(
                kInvalidOrBusyFrameTag, std::memory_order_relaxed);
        }
        sampleBits_ = std::move(sampleReplacement);
        frameTags_ = std::move(tagReplacement);
        channels_ = channels;
        frameCapacity_ = frameCapacity;
    }
    ResetForStart();
    return true;
}

void TxPcmStagingRing::BindTelemetry(
    TxPcmStagingTelemetry* telemetry) noexcept {
    telemetry_ = telemetry;
    if (telemetry_) {
        telemetry_->oldestValidFrame.store(
            OldestValidFrame(), std::memory_order_relaxed);
        telemetry_->writtenEndFrame.store(
            WrittenEndFrame(), std::memory_order_relaxed);
    }
}

void TxPcmStagingRing::ResetForStart() noexcept {
    oldestValidFrame_.store(0, std::memory_order_relaxed);
    writtenEndFrame_.store(0, std::memory_order_relaxed);
    hasRange_.store(false, std::memory_order_release);
    if (frameTags_) {
        for (uint32_t frame = 0; frame < frameCapacity_; ++frame) {
            frameTags_[frame].store(
                kInvalidOrBusyFrameTag, std::memory_order_relaxed);
        }
    }
    if (telemetry_) {
        telemetry_->Reset();
    }
}

TxPcmStageResult TxPcmStagingRing::Stage(
    const TxPcmHostRingView& hostView) noexcept {
    if (!sampleBits_ || channels_ == 0 || frameCapacity_ == 0) {
        return TxPcmStageResult::kNotConfigured;
    }
    if (!hostView.interleavedFloat32 ||
        hostView.channels != channels_ ||
        hostView.frameCount == 0 || hostView.frameCapacity == 0 ||
        hostView.frameCount > hostView.frameCapacity ||
        hostView.firstFrame > UINT64_MAX - hostView.frameCount) {
        return TxPcmStageResult::kInvalidView;
    }

    const uint64_t incomingEnd = hostView.firstFrame + hostView.frameCount;
    const bool hadRange = hasRange_.load(std::memory_order_acquire);
    const uint64_t previousEnd =
        writtenEndFrame_.load(std::memory_order_acquire);

    if (hadRange && incomingEnd <= previousEnd) {
        if (telemetry_) {
            telemetry_->overlapFramesIgnored.fetch_add(
                hostView.frameCount, std::memory_order_relaxed);
        }
        return TxPcmStageResult::kDuplicate;
    }

    uint64_t copyStart = hostView.firstFrame;
    bool discontinuity = false;
    if (hadRange) {
        if (copyStart < previousEnd) {
            const uint64_t ignored = previousEnd - copyStart;
            copyStart = previousEnd;
            if (telemetry_) {
                telemetry_->overlapFramesIgnored.fetch_add(
                    ignored, std::memory_order_relaxed);
            }
        } else if (copyStart > previousEnd) {
            discontinuity = true;
        }
    }

    const uint64_t framesToCopy = incomingEnd - copyStart;
    if (framesToCopy == 0) {
        return TxPcmStageResult::kDuplicate;
    }

    for (uint64_t absoluteFrame = copyStart;
         absoluteFrame < incomingEnd;
         ++absoluteFrame) {
        const uint64_t sourceFrame =
            absoluteFrame % hostView.frameCapacity;
        const float* source =
            hostView.interleavedFloat32 + sourceFrame * hostView.channels;
        const uint64_t destinationFrame =
            absoluteFrame % frameCapacity_;
        const uint64_t destinationBase = destinationFrame * channels_;
        // Invalidate the old logical frame before changing any sample. A
        // reader accepts a copied frame only when this absolute tag is equal
        // both before and after its sample loads.
        frameTags_[destinationFrame].store(
            kInvalidOrBusyFrameTag, std::memory_order_release);
        for (uint32_t channel = 0; channel < channels_; ++channel) {
            const float sample = source[channel];
            sampleBits_[destinationBase + channel].store(
                std::bit_cast<uint32_t>(sample),
                std::memory_order_relaxed);
        }
        frameTags_[destinationFrame].store(
            absoluteFrame, std::memory_order_release);
    }

    const uint64_t segmentStart =
        (!hadRange || discontinuity) ? copyStart
                                    : oldestValidFrame_.load(
                                          std::memory_order_relaxed);
    const uint64_t capacityFloor =
        incomingEnd > frameCapacity_ ? incomingEnd - frameCapacity_ : 0;
    const uint64_t newOldest =
        segmentStart > capacityFloor ? segmentStart : capacityFloor;
    const uint64_t previousOldest =
        oldestValidFrame_.load(std::memory_order_relaxed);
    oldestValidFrame_.store(newOldest, std::memory_order_relaxed);
    writtenEndFrame_.store(incomingEnd, std::memory_order_release);
    hasRange_.store(true, std::memory_order_release);

    if (telemetry_) {
        telemetry_->writes.fetch_add(1, std::memory_order_relaxed);
        telemetry_->framesStaged.fetch_add(
            framesToCopy, std::memory_order_relaxed);
        if (discontinuity) {
            telemetry_->discontinuities.fetch_add(
                1, std::memory_order_relaxed);
        }
        if (hadRange && newOldest > previousOldest) {
            telemetry_->overwrittenFrames.fetch_add(
                newOldest - previousOldest, std::memory_order_relaxed);
        }
        telemetry_->oldestValidFrame.store(
            newOldest, std::memory_order_relaxed);
        telemetry_->writtenEndFrame.store(
            incomingEnd, std::memory_order_release);
    }
    return TxPcmStageResult::kStaged;
}

ASFW::Audio::Ports::TxPcmReadResult
TxPcmStagingRing::ReadFloat32Interleaved(
    const ASFW::Audio::Ports::TxPcmReadRequest& request,
    float* destination,
    uint32_t destinationSampleCapacity) const noexcept {
    using Result = ASFW::Audio::Ports::TxPcmReadResult;

    if (!sampleBits_ || !destination || request.frameCount == 0 ||
        request.channelCount == 0 ||
        static_cast<uint64_t>(request.sourceChannelOffset) +
                request.channelCount >
            channels_ ||
        request.firstFrame > UINT64_MAX - request.frameCount ||
        static_cast<uint64_t>(request.frameCount) * request.channelCount >
            destinationSampleCapacity) {
        CountRead(Result::kInvalidRequest);
        return Result::kInvalidRequest;
    }

    const uint64_t requestedEnd = request.firstFrame + request.frameCount;
    for (uint32_t attempt = 0; attempt < kReadAttempts; ++attempt) {
        const bool hasRange = hasRange_.load(std::memory_order_acquire);
        const uint64_t writtenEnd =
            writtenEndFrame_.load(std::memory_order_acquire);
        const uint64_t oldest =
            oldestValidFrame_.load(std::memory_order_acquire);

        if (!hasRange || requestedEnd > writtenEnd) {
            CountRead(Result::kNotYetWritten);
            return Result::kNotYetWritten;
        }
        if (request.firstFrame < oldest) {
            CountRead(Result::kStaleOverwritten);
            return Result::kStaleOverwritten;
        }

        bool stable = true;
        for (uint32_t frame = 0; frame < request.frameCount; ++frame) {
            const uint64_t absoluteFrame = request.firstFrame + frame;
            const uint64_t physicalFrame = absoluteFrame % frameCapacity_;
            const auto& frameTag = frameTags_[physicalFrame];
            if (frameTag.load(std::memory_order_acquire) != absoluteFrame) {
                stable = false;
                break;
            }
            const uint64_t sourceBase = physicalFrame * channels_;
            float* output = destination +
                            static_cast<uint64_t>(frame) *
                                request.channelCount;
            for (uint32_t channel = 0;
                 channel < request.channelCount;
                 ++channel) {
                const uint64_t sourceChannel =
                    static_cast<uint64_t>(request.sourceChannelOffset) +
                    channel;
                output[channel] = std::bit_cast<float>(
                    sampleBits_[sourceBase + sourceChannel].load(
                        std::memory_order_relaxed));
            }
            if (frameTag.load(std::memory_order_acquire) != absoluteFrame) {
                stable = false;
                break;
            }
        }
        if (stable) {
            CountRead(Result::kReady);
            return Result::kReady;
        }

        // A failed tag check may be an active wrap. Reclassify after the
        // producer's latest published range before reporting a retryable busy.
        const uint64_t latestWritten =
            writtenEndFrame_.load(std::memory_order_acquire);
        const uint64_t latestOldest =
            oldestValidFrame_.load(std::memory_order_acquire);
        if (requestedEnd > latestWritten) {
            CountRead(Result::kNotYetWritten);
            return Result::kNotYetWritten;
        }
        if (request.firstFrame < latestOldest) {
            CountRead(Result::kStaleOverwritten);
            return Result::kStaleOverwritten;
        }
    }

    CountRead(Result::kSnapshotBusy);
    return Result::kSnapshotBusy;
}

uint64_t TxPcmStagingRing::OldestValidFrame() const noexcept {
    return oldestValidFrame_.load(std::memory_order_acquire);
}

uint64_t TxPcmStagingRing::WrittenEndFrame() const noexcept {
    return writtenEndFrame_.load(std::memory_order_acquire);
}

void TxPcmStagingRing::CountRead(
    ASFW::Audio::Ports::TxPcmReadResult result) const noexcept {
    if (!telemetry_) {
        return;
    }
    switch (result) {
        case ASFW::Audio::Ports::TxPcmReadResult::kReady:
            telemetry_->readsReady.fetch_add(1, std::memory_order_relaxed);
            break;
        case ASFW::Audio::Ports::TxPcmReadResult::kNotYetWritten:
            telemetry_->readsNotYetWritten.fetch_add(
                1, std::memory_order_relaxed);
            break;
        case ASFW::Audio::Ports::TxPcmReadResult::kStaleOverwritten:
            telemetry_->readsStaleOverwritten.fetch_add(
                1, std::memory_order_relaxed);
            break;
        case ASFW::Audio::Ports::TxPcmReadResult::kSnapshotBusy:
            telemetry_->readsSnapshotBusy.fetch_add(
                1, std::memory_order_relaxed);
            break;
        case ASFW::Audio::Ports::TxPcmReadResult::kInvalidRequest:
            telemetry_->readsInvalid.fetch_add(1, std::memory_order_relaxed);
            break;
    }
}

} // namespace ASFW::Audio::Runtime
