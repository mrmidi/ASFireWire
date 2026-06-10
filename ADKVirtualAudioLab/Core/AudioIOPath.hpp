#pragma once

#include "../Protocols/Audio/DICE/DiceTxStreamEngine.hpp"
#include "../Protocols/Audio/AMDTP/AmdtpTypes.hpp"

#include <cstdint>

namespace ASFW::Driver {

class AudioIOPath final {
public:
    AudioIOPath() noexcept = default;

    void BindDiceTxEngine(Protocols::Audio::DICE::DiceTxStreamEngine* engine) noexcept;

    void SetOutputFormatFloat32(uint32_t channels,
                                uint32_t frameCapacity) noexcept;

    void HandleWriteEnd(const Protocols::Audio::AMDTP::HostAudioBufferView& output) noexcept;

private:
    Protocols::Audio::DICE::DiceTxStreamEngine* diceTxEngine_{nullptr};

    uint32_t outputChannels_{0};
    uint32_t outputFrameCapacity_{0};
};

} // namespace ASFW::Driver
