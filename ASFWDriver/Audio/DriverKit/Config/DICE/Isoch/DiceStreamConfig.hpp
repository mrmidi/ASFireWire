// SPDX-License-Identifier: Apache-2.0
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

    // First host buffer channel this stream encodes (de-interleave). Stream 0
    // reads from 0; for a multi-stream device the Nth stream reads from
    // N × pcmChannels. Single-stream devices keep 0.
    uint8_t sourceChannelOffset{0};
};

} // namespace ASFW::Isoch::Audio::DICE
