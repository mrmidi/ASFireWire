#pragma once

#include "AudioTransportControlBlock.hpp"

#include <cstdint>

namespace ASFW::Audio::Runtime {

struct DirectAudioBindingSnapshot {
    uint64_t generation{0};

    int32_t* inputBase{nullptr};
    uint64_t inputBytes{0};
    uint32_t inputFrames{0};
    uint32_t inputChannels{0};

    const float* outputBase{nullptr};
    uint64_t outputBytes{0};
    uint32_t outputFrames{0};
    uint32_t outputChannels{0};

    AudioTransportControlBlock* control{nullptr};
    uint32_t sampleRateHz{0};
    bool valid{false};

    [[nodiscard]] bool HasInput() const noexcept {
        return inputBase != nullptr && inputFrames > 0 && inputChannels > 0;
    }

    [[nodiscard]] bool HasOutput() const noexcept {
        return outputBase != nullptr && outputFrames > 0 && outputChannels > 0;
    }

    [[nodiscard]] bool IsValidDuplex() const noexcept {
        return HasInput() && HasOutput() && control != nullptr && sampleRateHz > 0 && valid;
    }
};

class IDirectAudioBindingSource {
public:
    virtual ~IDirectAudioBindingSource() = default;
    [[nodiscard]]
    virtual bool CopyDirectAudioBinding(DirectAudioBindingSnapshot& out) noexcept = 0;
};

} // namespace ASFW::Audio::Runtime
