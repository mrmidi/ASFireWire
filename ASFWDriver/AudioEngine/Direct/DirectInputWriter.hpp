#pragma once

#include "../../Audio/DriverKit/Runtime/AudioGraphBinding.hpp"

#include <atomic>
#include <cstdint>

namespace ASFW::AudioEngine::Direct {

class DirectInputWriter final {
public:
    DirectInputWriter() = default;

    void Bind(const ASFW::Audio::Runtime::AudioGraphBinding* binding) noexcept {
        binding_ = binding;
    }

    void Unbind() noexcept {
        binding_ = nullptr;
    }

    [[nodiscard]] bool IsBound() const noexcept {
        return binding_ != nullptr && binding_->HasInput();
    }

    [[nodiscard]] int32_t* Frame(uint64_t absoluteFrame) const noexcept {
        if (!IsBound()) {
            return nullptr;
        }

        return binding_->memory.InputFrame(absoluteFrame);
    }

    void PublishProducedEnd(uint64_t producedEndFrame) noexcept {
        if (!binding_ || !binding_->control) {
            return;
        }

        binding_->control->inputProducedEndFrame.store(producedEndFrame,
                                                       std::memory_order_release);
    }

private:
    const ASFW::Audio::Runtime::AudioGraphBinding* binding_{nullptr};
};

} // namespace ASFW::AudioEngine::Direct
