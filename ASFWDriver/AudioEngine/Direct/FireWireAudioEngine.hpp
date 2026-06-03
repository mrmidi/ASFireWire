#pragma once

#include "AudioClockPublisher.hpp"
#include "DirectInputWriter.hpp"
#include "DirectOutputReader.hpp"

#include "../../Audio/DriverKit/Runtime/AudioGraphBinding.hpp"

namespace ASFW::AudioEngine::Direct {

class FireWireAudioEngine final {
public:
    FireWireAudioEngine() = default;

    [[nodiscard]] bool Bind(const ASFW::Audio::Runtime::AudioGraphBinding& binding) noexcept {
        if (!binding.IsValid()) {
            Unbind();
            return false;
        }

        binding_ = binding;
        bound_ = true;

        inputWriter_.Bind(&binding_);
        outputReader_.Bind(&binding_);
        clockPublisher_.Bind(&binding_);

        return true;
    }

    void Unbind() noexcept {
        bound_ = false;

        inputWriter_.Unbind();
        outputReader_.Unbind();
        clockPublisher_.Unbind();

        binding_ = {};
    }

    [[nodiscard]] bool IsBound() const noexcept {
        return bound_ && binding_.IsValid();
    }

    [[nodiscard]] const ASFW::Audio::Runtime::AudioGraphBinding& Binding() const noexcept {
        return binding_;
    }

    [[nodiscard]] DirectInputWriter& InputWriter() noexcept {
        return inputWriter_;
    }

    [[nodiscard]] DirectOutputReader& OutputReader() noexcept {
        return outputReader_;
    }

    [[nodiscard]] AudioClockPublisher& ClockPublisher() noexcept {
        return clockPublisher_;
    }

private:
    ASFW::Audio::Runtime::AudioGraphBinding binding_{};
    bool bound_{false};

    DirectInputWriter inputWriter_{};
    DirectOutputReader outputReader_{};
    AudioClockPublisher clockPublisher_{};
};

} // namespace ASFW::AudioEngine::Direct
