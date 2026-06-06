// OutputCursorDiscipline.hpp
// ASFW - Pure helper for forward-only recovery of the isoch TX read cursor.
// Normal playback follows the hardware-derived sample timeline. This helper is
// reserved for stale/overwritten recovery and can never replay consumed frames.

#pragma once

#include <cstdint>

namespace ASFW::AudioEngine::Direct::Tx {

struct CursorDiscipline final {
    uint64_t newCursor;
    bool resynced;
};

// Preserve a monotonic consumer timeline. A producer update may move the desired
// cursor forward, but it must never pull an already-consumed cursor backward.
[[nodiscard]] constexpr CursorDiscipline
DisciplineOutputCursor(uint64_t cursor,
                       uint64_t writtenEnd,
                       uint64_t targetLead,
                       uint64_t deadband) noexcept {
    const uint64_t target = (writtenEnd > targetLead) ? (writtenEnd - targetLead) : 0;

    if (target > cursor && (target - cursor) > deadband) {
        return CursorDiscipline{target, true};
    }
    return CursorDiscipline{cursor, false};
}

} // namespace ASFW::AudioEngine::Direct::Tx
