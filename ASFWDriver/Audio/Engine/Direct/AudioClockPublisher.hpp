#pragma once

#include "../../DriverKit/Runtime/AudioGraphBinding.hpp"

#include <cstdint>

namespace ASFW::AudioEngine::Direct {

class AudioClockPublisher final {
public:
    AudioClockPublisher() = default;

    void Bind(const ASFW::Audio::Runtime::AudioGraphBinding* binding) noexcept {
        binding_ = binding;
    }

    void Unbind() noexcept {
        binding_ = nullptr;
    }

    // The controller-side clock leg enqueues raw grid anchors in shared memory.
    // A coalesced Nub action drains them in the AudioDriverKit process.
    [[nodiscard]] bool IsBound() const noexcept {
        return binding_ != nullptr &&
               binding_->control != nullptr;
    }

    [[nodiscard]] ASFW::Audio::Runtime::HostClockAnchorPublishResult Publish(
        uint64_t sampleFrame,
        uint64_t hostTicks,
        uint32_t hostNanosPerSampleQ8) noexcept;

private:
    const ASFW::Audio::Runtime::AudioGraphBinding* binding_{nullptr};
};

} // namespace ASFW::AudioEngine::Direct
