#pragma once

#include <cstdint>

namespace ASFW::Audio::Runtime {

enum class AudioSampleStorage : uint32_t {
    kUnknown = 0,
    kInt32Native = 1,
    kFloat32Native = 2,
};

struct AudioStreamMemory final {
    float* inputBase{nullptr};
    const float* outputBase{nullptr};

    uint32_t activeInputRingFrames{0};
    uint32_t activeOutputRingFrames{0};

    uint32_t inputChannels{0};
    uint32_t outputChannels{0};

    AudioSampleStorage storage{AudioSampleStorage::kFloat32Native};

    [[nodiscard]] bool HasInput() const noexcept {
        return inputBase != nullptr &&
               activeInputRingFrames > 0 &&
               inputChannels > 0;
    }

    [[nodiscard]] bool HasOutput() const noexcept {
        return outputBase != nullptr &&
               activeOutputRingFrames > 0 &&
               outputChannels > 0;
    }

    [[nodiscard]] bool IsValid() const noexcept {
        return HasInput() || HasOutput();
    }

    [[nodiscard]] float* InputFrame(uint64_t absoluteFrame) const noexcept {
        if (!HasInput()) {
            return nullptr;
        }

        const uint64_t frameIndex = absoluteFrame % activeInputRingFrames;
        return inputBase + (frameIndex * inputChannels);
    }

    [[nodiscard]] const float* OutputFrame(uint64_t absoluteFrame) const noexcept {
        if (!HasOutput()) {
            return nullptr;
        }

        const uint64_t frameIndex = absoluteFrame % activeOutputRingFrames;
        return outputBase + (frameIndex * outputChannels);
    }
};

} // namespace ASFW::Audio::Runtime
