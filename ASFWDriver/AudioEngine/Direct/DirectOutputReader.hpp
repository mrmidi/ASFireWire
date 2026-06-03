#pragma once

#include "../../Audio/DriverKit/Runtime/AudioGraphBinding.hpp"

#include <cstdint>
#include <limits>

namespace ASFW::AudioEngine::Direct {

class DirectOutputReader final {
public:
    DirectOutputReader() = default;

    void Bind(const ASFW::Audio::Runtime::AudioGraphBinding* binding) noexcept {
        binding_ = binding;
    }

    void Unbind() noexcept {
        binding_ = nullptr;
    }

    [[nodiscard]] bool IsBound() const noexcept {
        return binding_ != nullptr && binding_->HasOutput();
    }

    [[nodiscard]] const int32_t* Frame(uint64_t absoluteFrame) const noexcept {
        if (!IsBound()) {
            return nullptr;
        }

        return binding_->memory.OutputFrame(absoluteFrame);
    }

    [[nodiscard]] bool IsFrameRangeAvailable(uint64_t firstFrame,
                                             uint32_t frameCount) const noexcept {
        if (!IsBound() || frameCount == 0) {
            return false;
        }

        const auto* control = binding_->control;
        if (!control) {
            return false;
        }

        const uint64_t writtenEnd = control->client.OutputWrittenEndFrame();
        constexpr uint64_t kMaxFrame = std::numeric_limits<uint64_t>::max();
        if (firstFrame > (kMaxFrame - frameCount)) {
            return false;
        }

        return writtenEnd >= (firstFrame + frameCount);
    }

private:
    const ASFW::Audio::Runtime::AudioGraphBinding* binding_{nullptr};
};

} // namespace ASFW::AudioEngine::Direct
