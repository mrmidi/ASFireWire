// TxCompletionStampDrain.hpp
// ASFW - Which TX completion stamps an observer still owes the timeline.
//
// Transport pushes one completion stamp per completed packet into a fixed
// shared ring. An observer that reads only the newest stamp of each wake sees
// one packet in N and therefore submits one packet in N to the sample timeline
// -- and because the timeline publishes a zero-timestamp boundary only when the
// boundary falls inside the packet it was given, the boundaries inside the
// unread packets are never published at all. The cursor below is what turns
// "the newest packet" back into "every packet".
//
// This is arithmetic over a producer/consumer cursor and a ring capacity. It
// holds no driver state so it can be exercised directly.

#pragma once

#include <cstdint>

namespace ASFW::Audio::Runtime {

/// The half-open stamp range an observer should read, plus what it lost.
struct TxCompletionStampDrain final {
    uint64_t first{0};        ///< First stamp index to read.
    uint64_t last{0};         ///< End-exclusive.
    uint64_t missed{0};       ///< Stamps overwritten before they were read.
    bool     queueRestarted{false};  ///< The stamp count went backwards.

    [[nodiscard]] constexpr bool Empty() const noexcept { return first >= last; }
    [[nodiscard]] constexpr uint64_t Count() const noexcept {
        return Empty() ? 0 : last - first;
    }
};

/// Plan the next drain.
///
/// `stampCount` is the producer's monotonically increasing push count, which
/// restarts at zero when the queue is re-armed; a cursor above it therefore
/// means a restart, not progress, and must not be preserved or the whole next
/// stream reads as already drained. Stamps older than `ringSlots` behind the
/// count have been overwritten: they are reported as missed rather than read,
/// because a torn stamp would enter the clock as a plausible wrong number.
[[nodiscard]] constexpr TxCompletionStampDrain PlanTxCompletionStampDrain(
    uint64_t cursor, uint64_t stampCount, uint32_t ringSlots) noexcept {
    TxCompletionStampDrain drain{};
    drain.last = stampCount;
    if (stampCount == 0 || ringSlots == 0) {
        drain.queueRestarted = cursor > stampCount;
        drain.first = stampCount;
        drain.last = stampCount;
        return drain;
    }
    if (cursor > stampCount) {
        drain.queueRestarted = true;
        cursor = 0;
    }
    const uint64_t oldestReadable =
        stampCount > ringSlots ? stampCount - ringSlots : 0;
    if (cursor < oldestReadable) {
        drain.missed = oldestReadable - cursor;
        cursor = oldestReadable;
    }
    drain.first = cursor;
    return drain;
}

} // namespace ASFW::Audio::Runtime
