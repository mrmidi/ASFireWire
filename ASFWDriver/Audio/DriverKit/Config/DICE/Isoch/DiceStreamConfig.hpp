// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2026 ASFireWire Project
//
// DiceStreamConfig.hpp
// Isoch stream configuration parameters.

#pragma once

#include "../../../../Wire/AMDTP/AmdtpTypes.hpp"
#include <cstdint>

namespace ASFW::Isoch::Audio::DICE {

enum class DiceStreamDirection : uint8_t {
    HostToDevice = 0,
    DeviceToHost = 1,
};

struct DiceStreamConfig final {
    DiceStreamDirection direction{DiceStreamDirection::HostToDevice};

    uint32_t sampleRate{48000};
    Encoding::StreamMode streamMode{Encoding::StreamMode::kBlocking};

    uint8_t sid{0};
    uint8_t pcmChannels{2};
    uint8_t dbs{2};
    uint8_t midiSlots{0};

    uint8_t framesPerDataPacket{8};
    uint8_t fdf{0x02};
    uint8_t fmt{0x10};
};

} // namespace ASFW::Isoch::Audio::DICE
