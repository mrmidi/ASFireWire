#pragma once

#include "AudioStreamMemory.hpp"
#include "AudioTransportControlBlock.hpp"

#include <cstdint>

class IOUserAudioDevice;

namespace ASFW::Audio::Runtime {

enum class AudioStreamMode : uint32_t {
    kUnknown = 0,
    kNonBlocking = 1,
    kBlocking = 2,
};

enum class AudioWireFormat : uint32_t {
    kUnknown = 0,
    kAM824 = 1,
    kRawPcm24In32 = 2,
    // Mirrors ASFW::Encoding::AudioWireFormat::kMotuV2. The two enums are deliberately
    // separate (this one is part of the shared control block's ABI and carries
    // kUnknown), so a value added to either must be added to both.
    kMotuV2 = 3,
};

struct AudioGraphBinding final {
    uint64_t guid{0};

    uint32_t sampleRateHz{0};

    AudioStreamMemory memory{};
    AudioTransportControlBlock* control{nullptr};

    uint32_t deviceToHostAm824Slots{0};
    uint32_t hostToDeviceAm824Slots{0};

    AudioStreamMode streamMode{AudioStreamMode::kUnknown};
    AudioWireFormat hostToDeviceWireFormat{AudioWireFormat::kAM824};

    IOUserAudioDevice* audioDevice{nullptr};

    [[nodiscard]] bool HasInput() const noexcept {
        return memory.HasInput() && deviceToHostAm824Slots > 0;
    }

    [[nodiscard]] bool HasOutput() const noexcept {
        return memory.HasOutput() && hostToDeviceAm824Slots > 0;
    }

    [[nodiscard]] bool IsValid() const noexcept {
        return guid != 0 &&
               sampleRateHz > 0 &&
               control != nullptr &&
               audioDevice != nullptr &&
               memory.IsValid();
    }
};

} // namespace ASFW::Audio::Runtime
