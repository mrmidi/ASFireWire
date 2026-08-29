#include "PcmPublicationCache.hpp"

#include <bit>
#include <cstddef>
#include <limits>
#include <new>
#include <utility>

namespace ASFW::Audio::Runtime {

bool PcmPublicationCache::Configure(uint32_t channels,
                                    uint32_t frameCapacity) noexcept {
    if (channels == 0 || frameCapacity == 0) return false;
    const uint64_t sampleCount = static_cast<uint64_t>(channels) * frameCapacity;
    if (sampleCount > std::numeric_limits<size_t>::max() /
                          sizeof(std::atomic<uint32_t>)) {
        return false;
    }

    if (!sampleBits_ || channels_ != channels || frameCapacity_ != frameCapacity) {
        auto samples = std::unique_ptr<std::atomic<uint32_t>[]>{
            new (std::nothrow) std::atomic<uint32_t>[sampleCount]};
        auto sequences = std::unique_ptr<std::atomic<uint64_t>[]>{
            new (std::nothrow) std::atomic<uint64_t>[frameCapacity]};
        auto epochs = std::unique_ptr<std::atomic<uint64_t>[]>{
            new (std::nothrow) std::atomic<uint64_t>[frameCapacity]};
        auto frames = std::unique_ptr<std::atomic<uint64_t>[]>{
            new (std::nothrow) std::atomic<uint64_t>[frameCapacity]};
        if (!samples || !sequences || !epochs || !frames) return false;
        for (uint64_t i = 0; i < sampleCount; ++i) {
            samples[i].store(0, std::memory_order_relaxed);
        }
        for (uint32_t i = 0; i < frameCapacity; ++i) {
            sequences[i].store(0, std::memory_order_relaxed);
            epochs[i].store(0, std::memory_order_relaxed);
            frames[i].store(UINT64_MAX, std::memory_order_relaxed);
        }
        sampleBits_ = std::move(samples);
        frameSequences_ = std::move(sequences);
        frameEpochs_ = std::move(epochs);
        frameAbsoluteFrames_ = std::move(frames);
        channels_ = channels;
        frameCapacity_ = frameCapacity;
    }
    BeginEpoch(0);
    return true;
}

void PcmPublicationCache::BindTelemetry(PcmPublicationTelemetry* telemetry) noexcept {
    telemetry_ = telemetry;
    if (telemetry_) telemetry_->Reset(Epoch());
}

void PcmPublicationCache::BeginEpoch(uint64_t epoch) noexcept {
    epochTransitionSequence_.fetch_add(1, std::memory_order_acq_rel);
    // WriteEnd never waits: once the transition is odd, new callbacks fail
    // closed. This serialized non-RT transition only waits for a callback that
    // had already entered to finish publishing its bounded range.
    while (activePublishers_.load(std::memory_order_acquire) != 0) {
        std::atomic_signal_fence(std::memory_order_seq_cst);
    }
    epoch_.store(epoch, std::memory_order_relaxed);
    oldestValidFrame_.store(0, std::memory_order_relaxed);
    publishedEndFrame_.store(0, std::memory_order_relaxed);
    hasRange_.store(false, std::memory_order_release);
    if (frameSequences_) {
        for (uint32_t frame = 0; frame < frameCapacity_; ++frame) {
            const uint64_t old = frameSequences_[frame].load(
                std::memory_order_relaxed);
            frameSequences_[frame].store((old | 1U) + 1U,
                                         std::memory_order_relaxed);
            frameEpochs_[frame].store(0, std::memory_order_relaxed);
            frameAbsoluteFrames_[frame].store(UINT64_MAX,
                                               std::memory_order_relaxed);
        }
    }
    epochTransitionSequence_.fetch_add(1, std::memory_order_release);
    if (telemetry_) telemetry_->Reset(epoch);
}

PcmPublishResult PcmPublicationCache::Publish(
    const PcmPublicationView& view) noexcept {
    if (!sampleBits_ || channels_ == 0 || frameCapacity_ == 0) {
        return PcmPublishResult::NotConfigured;
    }
    const uint64_t transitionBefore =
        epochTransitionSequence_.load(std::memory_order_acquire);
    if ((transitionBefore & 1U) != 0U) {
        return PcmPublishResult::WrongEpoch;
    }
    activePublishers_.fetch_add(1, std::memory_order_acq_rel);
    struct PublisherScope final {
        std::atomic<uint32_t>& active;
        ~PublisherScope() { active.fetch_sub(1, std::memory_order_release); }
    } publisherScope{activePublishers_};
    if (epochTransitionSequence_.load(std::memory_order_acquire) !=
        transitionBefore) {
        return PcmPublishResult::WrongEpoch;
    }
    const uint64_t activeEpoch = epoch_.load(std::memory_order_acquire);
    if (view.epoch != activeEpoch) return PcmPublishResult::WrongEpoch;
    if (!view.interleavedFloat32 || view.channels != channels_ ||
        view.frameCount == 0 || view.frameCapacity == 0 ||
        view.frameCount > view.frameCapacity ||
        view.frameCount > frameCapacity_ ||
        view.firstFrame > UINT64_MAX - view.frameCount) {
        return PcmPublishResult::InvalidView;
    }

    const uint64_t incomingEnd = view.firstFrame + view.frameCount;
    const bool hadRange = hasRange_.load(std::memory_order_acquire);
    const uint64_t previousEnd = publishedEndFrame_.load(std::memory_order_acquire);
    if (hadRange && incomingEnd <= previousEnd) {
        if (telemetry_) {
            telemetry_->duplicateFrames.fetch_add(view.frameCount,
                                                   std::memory_order_relaxed);
        }
        return PcmPublishResult::Duplicate;
    }

    uint64_t copyStart = view.firstFrame;
    bool discontinuity = false;
    if (hadRange) {
        if (copyStart < previousEnd) {
            const uint64_t duplicate = previousEnd - copyStart;
            copyStart = previousEnd;
            if (telemetry_) {
                telemetry_->duplicateFrames.fetch_add(duplicate,
                                                       std::memory_order_relaxed);
            }
        } else if (copyStart > previousEnd) {
            discontinuity = true;
        }
    }
    const uint64_t framesToCopy = incomingEnd - copyStart;
    if (framesToCopy == 0) return PcmPublishResult::Duplicate;

    for (uint64_t absoluteFrame = copyStart; absoluteFrame < incomingEnd;
         ++absoluteFrame) {
        const uint64_t sourceFrame = absoluteFrame % view.frameCapacity;
        const float* source = view.interleavedFloat32 + sourceFrame * view.channels;
        const uint64_t physicalFrame = absoluteFrame % frameCapacity_;
        const uint64_t destinationBase = physicalFrame * channels_;
        const uint64_t oldSequence = frameSequences_[physicalFrame].load(
            std::memory_order_relaxed);
        const uint64_t writingSequence = oldSequence | 1U;
        frameSequences_[physicalFrame].store(writingSequence,
                                             std::memory_order_release);
        frameEpochs_[physicalFrame].store(view.epoch,
                                          std::memory_order_relaxed);
        frameAbsoluteFrames_[physicalFrame].store(
            absoluteFrame, std::memory_order_relaxed);
        for (uint32_t channel = 0; channel < channels_; ++channel) {
            sampleBits_[destinationBase + channel].store(
                std::bit_cast<uint32_t>(source[channel]),
                std::memory_order_relaxed);
        }
        frameSequences_[physicalFrame].store(writingSequence + 1U,
                                             std::memory_order_release);
    }

    if (view.epoch != epoch_.load(std::memory_order_acquire) ||
        epochTransitionSequence_.load(std::memory_order_acquire) !=
            transitionBefore) {
        return PcmPublishResult::WrongEpoch;
    }

    const uint64_t segmentStart = (!hadRange || discontinuity)
        ? copyStart : oldestValidFrame_.load(std::memory_order_relaxed);
    const uint64_t capacityFloor = incomingEnd > frameCapacity_
        ? incomingEnd - frameCapacity_ : 0;
    const uint64_t newOldest = segmentStart > capacityFloor
        ? segmentStart : capacityFloor;
    const uint64_t previousOldest =
        oldestValidFrame_.load(std::memory_order_relaxed);
    oldestValidFrame_.store(newOldest, std::memory_order_relaxed);
    publishedEndFrame_.store(incomingEnd, std::memory_order_release);
    hasRange_.store(true, std::memory_order_release);

    if (telemetry_) {
        telemetry_->publications.fetch_add(1, std::memory_order_relaxed);
        telemetry_->framesPublished.fetch_add(framesToCopy,
                                               std::memory_order_relaxed);
        if (discontinuity) {
            telemetry_->discontinuities.fetch_add(1, std::memory_order_relaxed);
        }
        if (hadRange && newOldest > previousOldest) {
            telemetry_->expiredFrames.fetch_add(newOldest - previousOldest,
                                                 std::memory_order_relaxed);
        }
        telemetry_->oldestValidFrame.store(newOldest,
                                            std::memory_order_relaxed);
        telemetry_->publishedEndFrame.store(incomingEnd,
                                             std::memory_order_release);
    }
    return PcmPublishResult::Published;
}

ASFW::Audio::Ports::PcmCopyResult PcmPublicationCache::CopyExact(
    const ASFW::Audio::Ports::TxPcmReadRequest& request,
    float* destination,
    uint32_t destinationSampleCapacity) const noexcept {
    using Result = ASFW::Audio::Ports::PcmCopyResult;
    if (!sampleBits_ || !destination || request.epoch == 0 ||
        request.frameCount == 0 || request.channelCount == 0 ||
        static_cast<uint64_t>(request.sourceChannelOffset) +
                request.channelCount > channels_ ||
        request.firstFrame > UINT64_MAX - request.frameCount ||
        static_cast<uint64_t>(request.frameCount) * request.channelCount >
            destinationSampleCapacity) {
        CountCopy(Result::InvalidRequest);
        return Result::InvalidRequest;
    }
    if (request.epoch != epoch_.load(std::memory_order_acquire)) {
        CountCopy(Result::WrongEpoch);
        return Result::WrongEpoch;
    }

    const uint64_t requestedEnd = request.firstFrame + request.frameCount;
    for (uint32_t attempt = 0; attempt < kCopyAttempts; ++attempt) {
        const uint64_t transitionBefore =
            epochTransitionSequence_.load(std::memory_order_acquire);
        if ((transitionBefore & 1U) != 0U) continue;
        if (request.epoch != epoch_.load(std::memory_order_acquire)) {
            CountCopy(Result::WrongEpoch);
            return Result::WrongEpoch;
        }
        const bool hasRange = hasRange_.load(std::memory_order_acquire);
        const uint64_t publishedEnd =
            publishedEndFrame_.load(std::memory_order_acquire);
        const uint64_t oldest = oldestValidFrame_.load(std::memory_order_acquire);
        if (!hasRange || requestedEnd > publishedEnd) {
            CountCopy(Result::NotYetPublished);
            return Result::NotYetPublished;
        }
        if (request.firstFrame < oldest) {
            CountCopy(Result::Expired);
            return Result::Expired;
        }

        bool stable = true;
        for (uint32_t frame = 0; frame < request.frameCount; ++frame) {
            const uint64_t absoluteFrame = request.firstFrame + frame;
            const uint64_t physicalFrame = absoluteFrame % frameCapacity_;
            const uint64_t before = frameSequences_[physicalFrame].load(
                std::memory_order_acquire);
            if ((before & 1U) != 0U ||
                frameEpochs_[physicalFrame].load(std::memory_order_relaxed) !=
                    request.epoch ||
                frameAbsoluteFrames_[physicalFrame].load(
                    std::memory_order_relaxed) != absoluteFrame) {
                stable = false;
                break;
            }
            const uint64_t sourceBase = physicalFrame * channels_;
            float* output = destination +
                static_cast<uint64_t>(frame) * request.channelCount;
            for (uint32_t channel = 0; channel < request.channelCount; ++channel) {
                const uint64_t sourceChannel =
                    static_cast<uint64_t>(request.sourceChannelOffset) + channel;
                output[channel] = std::bit_cast<float>(
                    sampleBits_[sourceBase + sourceChannel].load(
                        std::memory_order_relaxed));
            }
            if (frameSequences_[physicalFrame].load(
                    std::memory_order_acquire) != before) {
                stable = false;
                break;
            }
        }
        if (stable && transitionBefore ==
                epochTransitionSequence_.load(std::memory_order_acquire)) {
            CountCopy(Result::Ready);
            return Result::Ready;
        }

        if (request.epoch != epoch_.load(std::memory_order_acquire)) {
            CountCopy(Result::WrongEpoch);
            return Result::WrongEpoch;
        }
        const uint64_t latestEnd =
            publishedEndFrame_.load(std::memory_order_acquire);
        const uint64_t latestOldest =
            oldestValidFrame_.load(std::memory_order_acquire);
        if (requestedEnd > latestEnd) {
            CountCopy(Result::NotYetPublished);
            return Result::NotYetPublished;
        }
        if (request.firstFrame < latestOldest) {
            CountCopy(Result::Expired);
            return Result::Expired;
        }
    }

    CountCopy(Result::ConcurrentRewrite);
    return Result::ConcurrentRewrite;
}

uint64_t PcmPublicationCache::OldestValidFrame() const noexcept {
    return oldestValidFrame_.load(std::memory_order_acquire);
}

uint64_t PcmPublicationCache::PublishedEndFrame() const noexcept {
    return publishedEndFrame_.load(std::memory_order_acquire);
}

uint64_t PcmPublicationCache::Epoch() const noexcept {
    return epoch_.load(std::memory_order_acquire);
}

void PcmPublicationCache::CountCopy(
    ASFW::Audio::Ports::PcmCopyResult result) const noexcept {
    if (!telemetry_) return;
    switch (result) {
        case ASFW::Audio::Ports::PcmCopyResult::Ready:
            telemetry_->copiesReady.fetch_add(1, std::memory_order_relaxed);
            break;
        case ASFW::Audio::Ports::PcmCopyResult::NotYetPublished:
            telemetry_->copiesNotYetPublished.fetch_add(1,
                                                         std::memory_order_relaxed);
            break;
        case ASFW::Audio::Ports::PcmCopyResult::Expired:
            telemetry_->copiesExpired.fetch_add(1, std::memory_order_relaxed);
            break;
        case ASFW::Audio::Ports::PcmCopyResult::WrongEpoch:
            telemetry_->copiesWrongEpoch.fetch_add(1, std::memory_order_relaxed);
            break;
        case ASFW::Audio::Ports::PcmCopyResult::ConcurrentRewrite:
            telemetry_->copiesConcurrentRewrite.fetch_add(
                1, std::memory_order_relaxed);
            break;
        case ASFW::Audio::Ports::PcmCopyResult::InvalidRequest:
            telemetry_->copiesInvalid.fetch_add(1, std::memory_order_relaxed);
            break;
    }
}

} // namespace ASFW::Audio::Runtime
