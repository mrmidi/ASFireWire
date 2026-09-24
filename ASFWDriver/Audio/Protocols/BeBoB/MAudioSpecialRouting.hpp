// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstdint>

namespace ASFW::Audio::BeBoB {

struct MAudioSpecialRoutingWrite final {
    uint16_t addressHi{0};
    uint32_t addressLo{0};
    std::array<uint8_t, 12> bytes{};
};

/// Minimal playback routing for the special M-Audio firmware. This writes only
/// the three route quadlets: stream pairs to their corresponding mixer pairs,
/// each headphone pair to its mixer pair, and analog outputs to the mixer.
/// Physical inputs and all gain/mute parameters are left untouched.
///
/// Values follow the measured 1814 playback route and the ALSA userspace
/// special mixer stream-pair defaults. This intentionally does not reproduce
/// the broader mixer/control initialization image.
[[nodiscard]] constexpr MAudioSpecialRoutingWrite
BuildMAudioSpecialRoutingWrite() noexcept {
    return {
        .addressHi = 0xFFC7,
        .addressLo = 0x00700094,
        .bytes = {0x00, 0x00, 0x00, 0x09, // stream -> mixer
                  0x00, 0x02, 0x00, 0x01, // headphone sources
                  0x00, 0x00, 0x00, 0x00}, // analog output sources
    };
}

} // namespace ASFW::Audio::BeBoB
