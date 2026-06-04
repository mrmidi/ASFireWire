// OutputCursorDiscipline.hpp
// ASFW - Pure helper that keeps the isoch TX read cursor a stable distance behind
// the HAL's write cursor (writtenEnd). Producer (writtenEnd) is driven by the ZTS
// clock; the consumer advances on the OHCI clock, so the two drift. We hold a target
// lead and rebase only when the observed lead leaves a deadband around it. See FW-26
// #6/#8. No DriverKit dependencies — unit-testable under ASFW_HOST_TEST.

#pragma once

#include <cstdint>

namespace ASFW::AudioEngine::Direct::Tx {

struct CursorDiscipline final {
    uint64_t newCursor;
    bool resynced;
};

// Keep (writtenEnd - cursor) ~= targetLead. Rebase to writtenEnd - targetLead only
// when the absolute lead error exceeds deadband; otherwise leave the cursor for the
// caller's per-packet advance. Clamps to 0 when writtenEnd < targetLead.
[[nodiscard]] constexpr CursorDiscipline
DisciplineOutputCursor(uint64_t cursor,
                       uint64_t writtenEnd,
                       uint64_t targetLead,
                       uint64_t deadband) noexcept {
    const uint64_t target = (writtenEnd > targetLead) ? (writtenEnd - targetLead) : 0;

    // Signed lead error in frames: how far the actual gap is from the target.
    // lead = writtenEnd - cursor; positive error means too little lead (near underrun).
    uint64_t errMagnitude;
    if (cursor <= writtenEnd) {
        const uint64_t lead = writtenEnd - cursor;
        errMagnitude = (lead > targetLead) ? (lead - targetLead) : (targetLead - lead);
    } else {
        // Cursor has overtaken writtenEnd entirely (severe underrun) — always rebase.
        errMagnitude = deadband + 1;
    }

    if (errMagnitude > deadband) {
        return CursorDiscipline{target, true};
    }
    return CursorDiscipline{cursor, false};
}

} // namespace ASFW::AudioEngine::Direct::Tx
