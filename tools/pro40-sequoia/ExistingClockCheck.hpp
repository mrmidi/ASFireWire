// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>

namespace Pro40 {
// Diagnostic policy, not a replacement for Linux's mandatory refresh. Compared
// with upstream 0e1661c0 DiceFamilyDriver.cpp:447-480,652-701. In this experiment
// any mismatch aborts; there is no fallback write that would change its meaning.
struct ExistingClockSample {
    uint64_t owner;
    uint32_t enabled, selected, status, measured, capabilities;
};
enum class ExistingClockResult { verified, cancelled, io, mismatch, deadline };
template<class Read, class Guard, class Cancelled, class Now, class Wait>
ExistingClockResult verifyExisting48k(uint64_t expectedOwner, Read read, Guard guard,
                                     Cancelled cancelled, Now now, Wait wait) {
    const auto begin = now();
    auto elapsed = [&] { return now() - begin; };
    unsigned agreeing = 0;
    while (true) {
        if (cancelled()) return ExistingClockResult::cancelled;
        if (elapsed() >= 3000000) return ExistingClockResult::deadline;
        if (!guard()) return ExistingClockResult::io;
        ExistingClockSample sample{};
        if (!read(sample)) return ExistingClockResult::io;
        if (!guard()) return ExistingClockResult::io;
        if (cancelled()) return ExistingClockResult::cancelled;
        if (elapsed() >= 3000000) return ExistingClockResult::deadline;
        if (expectedOwner == 0xffff000000000000ULL || sample.owner != expectedOwner ||
            sample.enabled != 0 || sample.selected != 0x20c || sample.status != 0x201 ||
            sample.measured != 48000 || !(sample.capabilities & (1u << 2)))
            return ExistingClockResult::mismatch;
        if (++agreeing == 3) return ExistingClockResult::verified;
        wait(); // Native adapter services the notification run loop for 20 ms.
    }
}
}
