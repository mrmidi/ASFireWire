// SPDX-License-Identifier: LGPL-3.0-or-later
// Copyright (c) 2026 ASFireWire Project
//
// DiceQuirks.hpp
// Quirks and slot encoding settings for DICE devices.

#pragma once

#include "../../../../Wire/AMDTP/AmdtpTypes.hpp"
#include <cstdint>

namespace ASFW::Isoch::Audio::DICE {

enum class DbsPolicy : uint8_t {
    Constant = 0,
    VariablePerPacket = 1
};

struct DiceTxQuirks final {
    Encoding::AudioWireFormat hostToDevicePcmEncoding{
        Encoding::AudioWireFormat::kAM824
    };

    DbsPolicy dbsPolicy{
        DbsPolicy::Constant
    };

    uint32_t defaultNonAudioSlotWord{0x80000000};
    bool initializeNonAudioSlots{true};
};

struct DiceRxQuirks final {
    Encoding::AudioWireFormat deviceToHostPcmEncoding{
        Encoding::AudioWireFormat::kAM824
    };

    DbsPolicy dbsPolicy{
        DbsPolicy::Constant
    };
};

struct DiceDeviceQuirks final {
    DiceTxQuirks tx{};
    DiceRxQuirks rx{};
};

} // namespace ASFW::Isoch::Audio::DICE
