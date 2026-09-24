// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// AmdtpTransferDelay.hpp -- IEC 61883-6 transfer delay derived from the wire
// geometry, in 24.576 MHz FireWire ticks.
//
// The derivation follows the Linux ALSA firewire stack as recorded by the
// midi-branch port (sound/firewire/amdtp-stream.c:302-308; not re-verified in
// this tree -- references/ was absent when this was written): a base device
// buffering of
// TRANSFER_DELAY_TICKS (0x2E00) minus one cycle (3072), plus -- in blocking
// mode -- one SYT interval of frames at the stream rate to absorb NO-DATA
// packets. Behaviour only; nothing is copied. Cross-check against
// references/linux-sound-firewire-stack/amdtp-stream.c before changing it.
//
// At 48/96/192 kHz blocking this is 12800 ticks, the value main has always
// sent. At 32 kHz and the 44.1 kHz family it is not; see
// ResolvedTimingGeometry.hpp (AppliedTransferDelayTicks) for why main does
// not apply it there yet.

#pragma once

#include "AmdtpRateGeometry.hpp"
#include "AmdtpTypes.hpp"

#include <cstdint>

namespace ASFW::Encoding {

inline constexpr uint32_t kAmdtpTicksPerCycle = 3'072;
inline constexpr uint64_t kAmdtpTicksPerSecond = 24'576'000;
/// Linux TRANSFER_DELAY_TICKS: base device buffering before the cycle term.
inline constexpr uint32_t kAmdtpBaseTransferDelayTicks = 0x2E00;

/// Transfer delay for a stream of the given rate geometry and mode.
[[nodiscard]] constexpr uint32_t AmdtpTransferDelayTicks(
    const AmdtpRateGeometry& geometry, StreamMode mode) noexcept {
    if (geometry.sampleRateHz == 0) {
        return 0;
    }
    constexpr uint32_t kBase = kAmdtpBaseTransferDelayTicks - kAmdtpTicksPerCycle;
    if (mode == StreamMode::kNonBlocking) {
        return kBase;
    }
    return kBase + static_cast<uint32_t>(
                       (kAmdtpTicksPerSecond * geometry.sytIntervalFrames) /
                       geometry.sampleRateHz);
}

namespace detail {
[[nodiscard]] constexpr uint32_t BlockingDelayAt(uint32_t rateHz) noexcept {
    const auto geometry = AmdtpRateGeometryForSampleRate(rateHz);
    return geometry ? AmdtpTransferDelayTicks(*geometry, StreamMode::kBlocking) : 0;
}
} // namespace detail

static_assert(detail::BlockingDelayAt(48'000) == 12'800);
static_assert(detail::BlockingDelayAt(96'000) == 12'800);
static_assert(detail::BlockingDelayAt(192'000) == 12'800);
static_assert(detail::BlockingDelayAt(44'100) == 13'162);
static_assert(detail::BlockingDelayAt(32'000) == 14'848);
static_assert(AmdtpTransferDelayTicks(*AmdtpRateGeometryForSampleRate(48'000),
                                      StreamMode::kNonBlocking) == 8'704);

} // namespace ASFW::Encoding
