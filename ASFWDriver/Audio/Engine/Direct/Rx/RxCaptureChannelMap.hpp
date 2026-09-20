// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// RxCaptureChannelMap.hpp — how one device's AM824 capture slots land on
// CoreAudio channels.
//
// This is the neutral shape only: which slot feeds which channel, and how far
// a channel must be delayed to undo a capture skew inside the device. It
// carries no device identity — the tables live with the family that needs them
// (see Audio/Protocols/BeBoB/BeBoBCaptureChannelMap.hpp) and arrive here
// already chosen.
//
// A default-constructed map is the identity: slot N feeds channel N with no
// delay, which is what every device that reports an honest channel order gets.

#pragma once

#include "../../../Wire/AMDTP/PcmSlotMap.hpp"

#include <cstdint>

namespace ASFW::AudioEngine::Direct::Rx {

struct RxCaptureChannelMap final : ASFW::Audio::Wire::PcmSlotMap {

    /// Frames of delay applied to the channels named by `delayedChannelMask`.
    /// Zero disables the delay path entirely.
    uint32_t delayFrames{0};

    /// Bit N set means CoreAudio channel N is delayed by `delayFrames`.
    uint32_t delayedChannelMask{0};

    [[nodiscard]] constexpr bool IsIdentity() const noexcept {
        return PcmSlotMap::IsIdentity() && delayFrames == 0;
    }

    [[nodiscard]] constexpr bool HasDelay() const noexcept {
        return delayFrames != 0 && delayedChannelMask != 0;
    }

    [[nodiscard]] constexpr bool IsDelayed(uint32_t channel) const noexcept {
        return channel < 32U && ((delayedChannelMask >> channel) & 1U) != 0U;
    }
};

} // namespace ASFW::AudioEngine::Direct::Rx
