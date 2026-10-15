// SPDX-License-Identifier: Apache-2.0
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

// The PCM encodings and the NO-DATA FDF are not quirks: every DICE device gets
// the same ones (IDiceDeviceProfile::kTxPcmEncoding, kRxPcmEncoding).
struct DiceTxQuirks final {
    DbsPolicy dbsPolicy{
        DbsPolicy::Constant
    };

    uint32_t defaultNonAudioSlotWord{0x80000000};
    bool initializeNonAudioSlots{true};
};

struct DiceRxQuirks final {
    DbsPolicy dbsPolicy{
        DbsPolicy::Constant
    };
};

struct DiceDeviceQuirks final {
    DiceTxQuirks tx{};
    DiceRxQuirks rx{};
};

} // namespace ASFW::Isoch::Audio::DICE
