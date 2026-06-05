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

    void PublishProducedEnd(uint64_t producedEndFrame, uint32_t decodedFrames = 0) noexcept {
        if (!binding_ || !binding_->control) {
            return;
        }

        auto* control = binding_->control;
        control->inputProducedEndFrame.store(producedEndFrame,
                                             std::memory_order_release);
        control->captureRingWriteFrame.store(producedEndFrame,
                                             std::memory_order_release);
        if (decodedFrames != 0) {
            control->counters.rxDecodedFrames.fetch_add(decodedFrames,
                                                        std::memory_order_relaxed);
        }

        const uint64_t read =
            control->captureRingReadFrame.load(std::memory_order_acquire);
        const uint32_t capacity = binding_->memory.inputFrameCapacity;
        if (capacity != 0 && producedEndFrame > read &&
            (producedEndFrame - read) > capacity) {
            control->captureRingReadFrame.store(producedEndFrame - capacity,
                                                std::memory_order_release);
            control->captureRingOverruns.fetch_add(1, std::memory_order_relaxed);
        }
    }

private:
    const ASFW::Audio::Runtime::AudioGraphBinding* binding_{nullptr};
};

} // namespace ASFW::AudioEngine::Direct
