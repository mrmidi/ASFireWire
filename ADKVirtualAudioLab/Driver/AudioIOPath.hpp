#pragma once

#include "../Protocols/Audio/DICE/DiceTxStreamEngine.hpp"

#include <cstdint>

namespace ASFW::Driver {

struct OutputBufferView final {
    const float* interleavedFloat32{nullptr};

    uint64_t sampleTime{0};
    uint32_t frameCount{0};
    uint32_t frameCapacity{0};
    uint32_t channels{0};
};

class AudioIOPath final {
public:
    AudioIOPath() noexcept = default;

    void BindDiceTxEngine(Protocols::Audio::DICE::DiceTxStreamEngine* engine) noexcept;

    void SetOutputFormatFloat32(uint32_t channels,
                                uint32_t frameCapacity) noexcept;

    void HandleWriteEnd(const OutputBufferView& output) noexcept;

private:
    Protocols::Audio::DICE::DiceTxStreamEngine* diceTxEngine_{nullptr};

    uint32_t outputChannels_{0};
    uint32_t outputFrameCapacity_{0};
};

} // namespace ASFW::Driver