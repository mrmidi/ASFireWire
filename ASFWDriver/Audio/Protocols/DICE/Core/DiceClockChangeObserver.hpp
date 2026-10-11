// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// DiceClockChangeObserver.hpp - Device steps around a DICE clock change.
//
// DiceFamilyDriver calls these synchronously, on its own thread, only around a
// CLOCK_SELECT write it actually issues (a redundant write that is skipped calls
// neither). Both are device configuration, not bring-up gates: a failure is the
// observer's to log, and the bring-up continues.

#pragma once

#include <cstdint>

namespace ASFW::Audio::DICE {

class DiceDeviceIo;

class DiceClockChangeObserver {
public:
    virtual ~DiceClockChangeObserver() = default;

    // Before CLOCK_SELECT is written, owner already claimed.
    virtual void BeforeClockSelectWrite(DiceDeviceIo& io) noexcept { (void)io; }

    // After the device runs at `rateHz`. `rateChanged` is false when the device
    // already ran at that rate before the write.
    virtual void AfterClockChange(DiceDeviceIo& io, uint32_t rateHz, bool rateChanged) noexcept {
        (void)io;
        (void)rateHz;
        (void)rateChanged;
    }
};

} // namespace ASFW::Audio::DICE
