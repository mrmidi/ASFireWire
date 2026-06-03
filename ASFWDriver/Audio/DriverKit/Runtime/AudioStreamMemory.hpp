#pragma once

#include <cstdint>

namespace ASFW::Audio::Runtime {

enum class AudioSampleStorage : uint32_t {
    kUnknown = 0,
    kInt32Native = 1,
};

struct AudioStreamMemory final {
    int32_t* inputBase{nullptr};
    const int32_t* outputBase{nullptr};

    uint32_t inputFrameCapacity{0};
    uint32_t outputFrameCapacity{0};

    uint32_t inputChannels{0};
    uint32_t outputChannels{0};

    AudioSampleStorage storage{AudioSampleStorage::kInt32Native};

    [[nodiscard]] bool HasInput() const noexcept {
        return inputBase != nullptr &&
               inputFrameCapacity > 0 &&
               inputChannels > 0;
    }

    [[nodiscard]] bool HasOutput() const noexcept {
        return outputBase != nullptr &&
               outputFrameCapacity > 0 &&
               outputChannels > 0;
    }

    [[nodiscard]] bool IsValid() const noexcept {
        return HasInput() || HasOutput();
    }

    [[nodiscard]] int32_t* InputFrame(uint64_t absoluteFrame) const noexcept {
        if (!HasInput()) {
            return nullptr;
        }

        const uint64_t frameIndex = absoluteFrame % inputFrameCapacity;
        return inputBase + (frameIndex * inputChannels);
    }

    [[nodiscard]] const int32_t* OutputFrame(uint64_t absoluteFrame) const noexcept {
        if (!HasOutput()) {
            return nullptr;
        }

        const uint64_t frameIndex = absoluteFrame % outputFrameCapacity;
        return outputBase + (frameIndex * outputChannels);
    }
};

} // namespace ASFW::Audio::Runtime
