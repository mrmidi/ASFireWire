#pragma once

#include <cstdint>

namespace ASFW::AudioEngine::Direct::Rx {

class ZtsFrameProjector final {
public:
    explicit constexpr ZtsFrameProjector(uint64_t periodFrames) noexcept
        : periodFrames_(periodFrames) {}

    void Reset(uint64_t absoluteFrame) noexcept {
        absoluteFrame_ = absoluteFrame;
        nextBoundaryFrame_ =
            periodFrames_ == 0
                ? 0
                : ((absoluteFrame / periodFrames_) + 1) * periodFrames_;
    }

    template <typename CrossingCallback>
    void Advance(uint32_t decodedFrames,
                 CrossingCallback&& onCrossing) noexcept {
        const uint64_t firstFrame = absoluteFrame_;
        absoluteFrame_ += decodedFrames;
        if (periodFrames_ == 0) {
            return;
        }

        while (nextBoundaryFrame_ <= absoluteFrame_) {
            const uint64_t offset = nextBoundaryFrame_ - firstFrame;
            onCrossing(
                nextBoundaryFrame_,
                static_cast<uint32_t>(offset));
            nextBoundaryFrame_ += periodFrames_;
        }
    }

    [[nodiscard]] uint64_t AbsoluteFrame() const noexcept {
        return absoluteFrame_;
    }

    [[nodiscard]] uint64_t NextBoundaryFrame() const noexcept {
        return nextBoundaryFrame_;
    }

private:
    uint64_t periodFrames_{0};
    uint64_t absoluteFrame_{0};
    uint64_t nextBoundaryFrame_{0};
};

} // namespace ASFW::AudioEngine::Direct::Rx
