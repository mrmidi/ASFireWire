// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2026 ASFireWire Project

#pragma once

#include "../Config/AudioConstants.hpp"
#include "../Wire/AMDTP/AmdtpRateGeometry.hpp"

#include <DriverKit/IOLib.h>

#include <atomic>
#include <cstdint>
#include <cstring>

namespace ASFW::Audio::Runtime {

struct AudioRingWriteResult final {
    uint64_t firstFrame{0};
    uint64_t endFrame{0};
    uint32_t requestedFrames{0};
    uint32_t copiedFrames{0};
    bool overrun{false};
    bool invalid{false};
};

struct AudioRingReadResult final {
    uint64_t firstFrame{0};
    uint64_t endFrame{0};
    uint32_t requestedFrames{0};
    uint32_t copiedFrames{0};
    bool underrun{false};
    bool starvation{false};
    bool invalid{false};
};

struct AudioSampleRingCounters final {
    std::atomic<uint64_t>* underruns{nullptr};
    std::atomic<uint64_t>* overruns{nullptr};
    std::atomic<uint64_t>* starvations{nullptr};
};

class AudioSampleRing final {
public:
    AudioSampleRing() noexcept = default;

    ~AudioSampleRing() noexcept {
        ReleaseOwnedStorage();
    }

    AudioSampleRing(const AudioSampleRing&) = delete;
    AudioSampleRing& operator=(const AudioSampleRing&) = delete;

    [[nodiscard]] kern_return_t Configure(uint32_t capacityFrames,
                                          uint32_t channels) noexcept {
        if (!IsPowerOfTwo(capacityFrames) ||
            channels == 0 ||
            channels > ASFW::Isoch::Config::kMaxPcmChannels) {
            return kIOReturnBadArgument;
        }

        const uint64_t sampleCount = static_cast<uint64_t>(capacityFrames) * channels;
        const uint64_t byteCount = sampleCount * sizeof(int32_t);
        if (byteCount == 0 || sampleCount > UINT32_MAX) {
            return kIOReturnBadArgument;
        }

        if (ownedStorage_ &&
            capacityFrames_ == capacityFrames &&
            channels_ == channels &&
            storage_ != nullptr) {
            Reset(0);
            return kIOReturnSuccess;
        }

        ReleaseOwnedStorage();
        auto* storage = static_cast<int32_t*>(IOMallocZero(static_cast<size_t>(byteCount)));
        if (!storage) {
            return kIOReturnNoMemory;
        }

        storage_ = storage;
        storageBytes_ = byteCount;
        capacityFrames_ = capacityFrames;
        channels_ = channels;
        ownedStorage_ = true;
        Reset(0);
        return kIOReturnSuccess;
    }

    [[nodiscard]] kern_return_t BindExternal(int32_t* storage,
                                             uint32_t capacityFrames,
                                             uint32_t channels,
                                             std::atomic<uint64_t>* writeFrame,
                                             std::atomic<uint64_t>* readFrame,
                                             AudioSampleRingCounters counters = {}) noexcept {
        if (!storage ||
            !writeFrame ||
            !readFrame ||
            !IsPowerOfTwo(capacityFrames) ||
            channels == 0 ||
            channels > ASFW::Encoding::kMaxPcmChannels) {
            return kIOReturnBadArgument;
        }

        ReleaseOwnedStorage();
        storage_ = storage;
        storageBytes_ = static_cast<uint64_t>(capacityFrames) * channels * sizeof(int32_t);
        capacityFrames_ = capacityFrames;
        channels_ = channels;
        ownedStorage_ = false;
        writeFrame_ = writeFrame;
        readFrame_ = readFrame;
        counters_ = counters;
        return kIOReturnSuccess;
    }

    void Unbind() noexcept {
        ReleaseOwnedStorage();
        storage_ = nullptr;
        storageBytes_ = 0;
        capacityFrames_ = 0;
        channels_ = 0;
        ownedStorage_ = false;
        writeFrame_ = &ownedWriteFrame_;
        readFrame_ = &ownedReadFrame_;
        counters_ = {};
    }

    void Reset(uint64_t startFrame) noexcept {
        if (storage_ && storageBytes_ != 0) {
            std::memset(storage_, 0, static_cast<size_t>(storageBytes_));
        }
        writeFrame_->store(startFrame, std::memory_order_release);
        readFrame_->store(startFrame, std::memory_order_release);
        ownedUnderruns_.store(0, std::memory_order_relaxed);
        ownedOverruns_.store(0, std::memory_order_relaxed);
        ownedStarvations_.store(0, std::memory_order_relaxed);
        if (counters_.underruns) {
            counters_.underruns->store(0, std::memory_order_relaxed);
        }
        if (counters_.overruns) {
            counters_.overruns->store(0, std::memory_order_relaxed);
        }
        if (counters_.starvations) {
            counters_.starvations->store(0, std::memory_order_relaxed);
        }
    }

    [[nodiscard]] bool IsConfigured() const noexcept {
        return storage_ != nullptr && capacityFrames_ != 0 && channels_ != 0;
    }

    [[nodiscard]] uint32_t CapacityFrames() const noexcept { return capacityFrames_; }
    [[nodiscard]] uint32_t Channels() const noexcept { return channels_; }
    [[nodiscard]] uint64_t WriteFrame() const noexcept {
        return writeFrame_->load(std::memory_order_acquire);
    }
    [[nodiscard]] uint64_t ReadFrame() const noexcept {
        return readFrame_->load(std::memory_order_acquire);
    }

    [[nodiscard]] uint32_t AvailableFramesAt(uint64_t firstFrame) const noexcept {
        if (!IsConfigured()) {
            return 0;
        }
        const uint64_t write = WriteFrame();
        if (firstFrame >= write) {
            return 0;
        }
        const uint64_t oldest = OldestAvailableFrame(write);
        if (firstFrame < oldest) {
            return 0;
        }
        const uint64_t available = write - firstFrame;
        return available > capacityFrames_ ? capacityFrames_ : static_cast<uint32_t>(available);
    }

    [[nodiscard]] AudioRingWriteResult WriteFrames(uint64_t firstFrame,
                                                   const int32_t* src,
                                                   uint32_t frameCount,
                                                   uint32_t srcChannels) noexcept {
        AudioRingWriteResult result{
            .firstFrame = firstFrame,
            .endFrame = firstFrame + frameCount,
            .requestedFrames = frameCount,
        };

        if (!IsConfigured() || !src || frameCount == 0 || srcChannels == 0) {
            result.invalid = frameCount != 0;
            return result;
        }

        uint64_t write = WriteFrame();
        uint32_t srcOffsetFrames = 0;
        uint32_t framesToCopy = frameCount;

        if (firstFrame < write) {
            const uint64_t staleFrames = write - firstFrame;
            if (staleFrames >= frameCount) {
                return result;
            }
            srcOffsetFrames = static_cast<uint32_t>(staleFrames);
            framesToCopy -= srcOffsetFrames;
            firstFrame = write;
        } else if (firstFrame > write) {
            write = firstFrame;
            writeFrame_->store(write, std::memory_order_release);
        }

        for (uint32_t frame = 0; frame < framesToCopy; ++frame) {
            int32_t* dstFrame = MutableFrame(firstFrame + frame);
            const int32_t* srcFrame =
                src + (static_cast<size_t>(srcOffsetFrames + frame) * srcChannels);
            CopyFrame(dstFrame, srcFrame, srcChannels);
        }

        const uint64_t endFrame = firstFrame + framesToCopy;
        writeFrame_->store(endFrame, std::memory_order_release);
        result.firstFrame = firstFrame;
        result.endFrame = endFrame;
        result.copiedFrames = framesToCopy;

        const uint64_t read = ReadFrame();
        if (endFrame > read && (endFrame - read) > capacityFrames_) {
            readFrame_->store(endFrame - capacityFrames_, std::memory_order_release);
            result.overrun = true;
            Count(counters_.overruns, ownedOverruns_);
        }

        return result;
    }

    [[nodiscard]] AudioRingReadResult ReadFrames(uint64_t firstFrame,
                                                 int32_t* dst,
                                                 uint32_t frameCount,
                                                 uint32_t dstChannels,
                                                 bool zeroFillOnMiss = true) noexcept {
        AudioRingReadResult result{
            .firstFrame = firstFrame,
            .endFrame = firstFrame + frameCount,
            .requestedFrames = frameCount,
        };

        if (!IsConfigured() || !dst || frameCount == 0 || dstChannels == 0) {
            result.invalid = frameCount != 0;
            return result;
        }

        const uint64_t write = WriteFrame();
        const uint64_t oldest = OldestAvailableFrame(write);
        uint32_t copied = 0;

        for (uint32_t frame = 0; frame < frameCount; ++frame) {
            const uint64_t absoluteFrame = firstFrame + frame;
            int32_t* dstFrame = dst + (static_cast<size_t>(frame) * dstChannels);
            if (absoluteFrame >= oldest && absoluteFrame < write) {
                CopyFrame(dstFrame, Frame(absoluteFrame), channels_, dstChannels);
                ++copied;
            } else if (zeroFillOnMiss) {
                ZeroFrame(dstFrame, dstChannels);
            }
        }

        result.copiedFrames = copied;
        if (copied < frameCount) {
            result.underrun = true;
            result.starvation = true;
            Count(counters_.underruns, ownedUnderruns_);
            Count(counters_.starvations, ownedStarvations_);
        }

        const uint64_t endFrame = firstFrame + frameCount;
        const uint64_t prevRead = ReadFrame();
        if (endFrame > prevRead) {
            readFrame_->store(endFrame, std::memory_order_release);
        }
        return result;
    }

    void PublishWriteEnd(uint64_t endFrame) noexcept {
        const uint64_t prev = WriteFrame();
        if (endFrame > prev) {
            writeFrame_->store(endFrame, std::memory_order_release);
            const uint64_t read = ReadFrame();
            if ((endFrame - read) > capacityFrames_) {
                readFrame_->store(endFrame - capacityFrames_, std::memory_order_release);
                Count(counters_.overruns, ownedOverruns_);
            }
        }
    }

    void PublishReadEnd(uint64_t endFrame) noexcept {
        const uint64_t prev = ReadFrame();
        if (endFrame > prev) {
            readFrame_->store(endFrame, std::memory_order_release);
        }
    }

    [[nodiscard]] const int32_t* Frame(uint64_t absoluteFrame) const noexcept {
        if (!IsConfigured()) {
            return nullptr;
        }
        return storage_ + ((absoluteFrame & (capacityFrames_ - 1u)) * channels_);
    }

    [[nodiscard]] int32_t* MutableFrame(uint64_t absoluteFrame) noexcept {
        if (!IsConfigured()) {
            return nullptr;
        }
        return storage_ + ((absoluteFrame & (capacityFrames_ - 1u)) * channels_);
    }

private:
    [[nodiscard]] static bool IsPowerOfTwo(uint32_t value) noexcept {
        return value != 0 && ((value & (value - 1u)) == 0);
    }

    [[nodiscard]] uint64_t OldestAvailableFrame(uint64_t write) const noexcept {
        return (write > capacityFrames_) ? (write - capacityFrames_) : 0;
    }

    static void Count(std::atomic<uint64_t>* external,
                      std::atomic<uint64_t>& owned) noexcept {
        if (external) {
            external->fetch_add(1, std::memory_order_relaxed);
        } else {
            owned.fetch_add(1, std::memory_order_relaxed);
        }
    }

    void CopyFrame(int32_t* dstFrame,
                   const int32_t* srcFrame,
                   uint32_t srcChannels,
                   uint32_t dstChannels = 0) const noexcept {
        if (!dstFrame || !srcFrame) {
            return;
        }
        const uint32_t copyChannels =
            srcChannels < (dstChannels == 0 ? channels_ : dstChannels)
                ? srcChannels
                : (dstChannels == 0 ? channels_ : dstChannels);
        for (uint32_t ch = 0; ch < copyChannels; ++ch) {
            dstFrame[ch] = srcFrame[ch];
        }
        const uint32_t outChannels = (dstChannels == 0) ? channels_ : dstChannels;
        for (uint32_t ch = copyChannels; ch < outChannels; ++ch) {
            dstFrame[ch] = 0;
        }
    }

    static void ZeroFrame(int32_t* frame, uint32_t channels) noexcept {
        if (frame && channels != 0) {
            std::memset(frame, 0, static_cast<size_t>(channels) * sizeof(int32_t));
        }
    }

    void ReleaseOwnedStorage() noexcept {
        if (ownedStorage_ && storage_) {
            IOFree(storage_, static_cast<size_t>(storageBytes_));
        }
        if (ownedStorage_) {
            storage_ = nullptr;
            storageBytes_ = 0;
            ownedStorage_ = false;
        }
    }

    int32_t* storage_{nullptr};
    uint64_t storageBytes_{0};
    uint32_t capacityFrames_{0};
    uint32_t channels_{0};
    bool ownedStorage_{false};

    std::atomic<uint64_t> ownedWriteFrame_{0};
    std::atomic<uint64_t> ownedReadFrame_{0};
    std::atomic<uint64_t> ownedUnderruns_{0};
    std::atomic<uint64_t> ownedOverruns_{0};
    std::atomic<uint64_t> ownedStarvations_{0};

    std::atomic<uint64_t>* writeFrame_{&ownedWriteFrame_};
    std::atomic<uint64_t>* readFrame_{&ownedReadFrame_};
    AudioSampleRingCounters counters_{};
};

} // namespace ASFW::Audio::Runtime
