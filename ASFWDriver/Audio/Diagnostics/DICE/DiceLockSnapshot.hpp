// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace ASFW::Audio::DICE::Diagnostics {

enum class DiceLockPhase : uint32_t { Idle, Watching, Complete, Stopped, Reset, Unavailable };

// Family-specific telemetry (selector 1015), independent of the generic audio ABI.
// Native little-endian scalar wire layout, copied under a lock; never a buffer view.
struct DiceLockSnapshot {
    uint32_t version{1};
    uint32_t bytes{96};
    uint64_t guid{0};
    uint32_t generation{0};
    DiceLockPhase phase{DiceLockPhase::Idle};
    uint32_t elapsedMs{0};
    uint32_t samples{0};
    uint32_t readErrors{0};
    uint32_t lastError{0};
    uint32_t status{0};
    uint32_t extStatus{0};
    uint32_t slipHistory{0};
    uint32_t expectedMask{0};
    uint32_t observedMask{0};
    uint32_t lostMask{0};
    struct Receiver {
        uint32_t channel{0xFFFFFFFFU};
        uint32_t firstLockMs{0xFFFFFFFFU};
    };
    std::array<Receiver, 4> receivers{};
};
static_assert(sizeof(DiceLockSnapshot) == 96);
#define ASFW_DICE_LOCK_OFFSET(field, offset) \
    static_assert(offsetof(DiceLockSnapshot, field) == offset)
ASFW_DICE_LOCK_OFFSET(version, 0);
ASFW_DICE_LOCK_OFFSET(bytes, 4);
ASFW_DICE_LOCK_OFFSET(guid, 8);
ASFW_DICE_LOCK_OFFSET(generation, 16);
ASFW_DICE_LOCK_OFFSET(phase, 20);
ASFW_DICE_LOCK_OFFSET(elapsedMs, 24);
ASFW_DICE_LOCK_OFFSET(samples, 28);
ASFW_DICE_LOCK_OFFSET(readErrors, 32);
ASFW_DICE_LOCK_OFFSET(lastError, 36);
ASFW_DICE_LOCK_OFFSET(status, 40);
ASFW_DICE_LOCK_OFFSET(extStatus, 44);
ASFW_DICE_LOCK_OFFSET(slipHistory, 48);
ASFW_DICE_LOCK_OFFSET(expectedMask, 52);
ASFW_DICE_LOCK_OFFSET(observedMask, 56);
ASFW_DICE_LOCK_OFFSET(lostMask, 60);
ASFW_DICE_LOCK_OFFSET(receivers, 64);
#undef ASFW_DICE_LOCK_OFFSET
static_assert(sizeof(DiceLockSnapshot::Receiver) == 8);
static_assert(offsetof(DiceLockSnapshot::Receiver, firstLockMs) == 4);

} // namespace ASFW::Audio::DICE::Diagnostics
