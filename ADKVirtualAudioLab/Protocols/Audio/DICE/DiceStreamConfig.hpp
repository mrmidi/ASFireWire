#pragma once

#include "../AMDTP/AmdtpTypes.hpp"

#include <cstdint>

namespace ASFW::Protocols::Audio::DICE {

enum class DiceStreamDirection : uint8_t {
    HostToDevice = 0,
    DeviceToHost = 1,
};

struct DiceStreamConfig final {
    DiceStreamDirection direction{DiceStreamDirection::HostToDevice};

    uint32_t sampleRate{48000};
    AMDTP::StreamMode streamMode{AMDTP::StreamMode::Blocking};

    uint8_t sid{0};
    uint8_t pcmChannels{2};
    uint8_t dbs{2};
    uint8_t midiSlots{0};

    uint8_t framesPerDataPacket{8};
    uint8_t fdf{0x02};
    uint8_t fmt{0x10};
};

class DiceStreamConfigMapper final {
public:
    [[nodiscard]] static AMDTP::AmdtpStreamConfig ToAmdtpConfig(
        const DiceStreamConfig& diceConfig) noexcept;
};

} // namespace ASFW::Protocols::Audio::DICE