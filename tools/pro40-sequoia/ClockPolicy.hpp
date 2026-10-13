// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>

namespace Pro40Clock {
// ALSA tcat/global_section.rs serialize_clock_config; Linux dice-stream.c:60-99.
constexpr uint32_t index(uint32_t hz) {
    switch (hz) {
    case 44100: return 1;
    case 48000: return 2;
    case 88200: return 3;
    case 96000: return 4;
    default: return 0xffffffff;
    }
}
constexpr bool permitted(uint32_t hz, uint32_t selected, uint32_t enabled,
                         uint32_t caps, uint64_t owner) {
    const auto i = index(hz);
    return i != 0xffffffff && enabled == 0 && owner == 0xffff000000000000ULL &&
        (selected & 0xff) == 12 && (caps & (1u << i)) != 0;
}
constexpr uint32_t select(uint32_t hz, uint32_t previous) {
    return (previous & ~0xff00u) | (index(hz) << 8);
}
// Linux dice-stream.c:91-98 tolerates missing notification after 100 ms only
// when CLOCK_SELECT was rewritten unchanged. Stable readback is additionally
// required by our native caller; changed selections still require acceptance.
constexpr bool accepted(bool unchanged, bool notified, bool invalidNotification,
                        unsigned stableReads, uint64_t elapsedMilliseconds) {
    return !invalidNotification && stableReads >= 3 &&
        (notified || (unchanged && elapsedMilliseconds >= 100));
}
}
