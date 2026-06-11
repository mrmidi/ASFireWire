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

    // The controller-side clock leg publishes into the shared transport control
    // block even when the IOUserAudioDevice lives in the AudioDriverKit process.
    // The ADK side mirrors this shared timeline to UpdateCurrentZeroTimestamp.
    [[nodiscard]] bool IsBound() const noexcept {
        return binding_ != nullptr &&
               binding_->control != nullptr;
    }

    void Publish(uint64_t sampleFrame,
                 uint64_t hostTicks,
                 uint32_t hostNanosPerSampleQ8) noexcept;

private:
    const ASFW::Audio::Runtime::AudioGraphBinding* binding_{nullptr};
};

} // namespace ASFW::AudioEngine::Direct
