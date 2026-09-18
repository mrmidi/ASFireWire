// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// MotuRxTiming.hpp - Capture-side presentation timing for MOTU protocol-v2 streams.
//
// Every other 61883-6 family this driver speaks carries presentation time in the CIP
// header's SYT field, which DirectAudioReceiveConsumer turns into
// RxSequenceEntry::sytOffset via ComputeReplaySytOffset(). MOTU does not: it sets the
// CIP SPH bit and puts a 32-bit source packet header at the head of *every data block*
// instead (amdtp-motu.c:19-25, 303-393).
//
// The replay contract is unchanged though -- RxSequenceEntry::sytOffset is a presentation
// offset relative to the packet's own arrival, whatever produced it. So MOTU populates
// the same field from a different source, and the existing TX replay path
// (ASFWAudioDriverZts.cpp) consumes it without knowing the difference.
//
// This header is the pure arithmetic for that: no I/O, no state, so the wire truth can
// be pinned by host tests. Rebasing for transmit lives in MotuSph.hpp (ReplaySph).

#pragma once

#include "MotuBlockCodec.hpp"
#include "MotuBlockLayout.hpp"
#include "MotuSph.hpp"
#include "../../../Common/TimingUtils.hpp"

#include <cstdint>
#include <span>

namespace ASFW::Encoding::Motu {

/// CIP header preceding the first data block, as on every 61883-6 stream.
inline constexpr uint32_t kCipHeaderBytes = 8;

struct MotuRxTiming final {
    /// False when the payload cannot hold a data block, so nothing was decoded.
    bool valid{false};
    /// Raw SPH quadlet of the first data block, kept for diagnostics.
    uint32_t firstBlockSph{0};
    /// Presentation offset in 24.576 MHz ticks, relative to the packet's own cycle
    /// timer. Feeds RxSequenceEntry::sytOffset unchanged.
    uint32_t presentationOffsetTicks{UINT32_MAX};
};

/// Tick on MOTU's one-second SPH timeline for a packet's cycle timer.
///
/// The SPH timeline wraps every second, so the cycle-timer *seconds* field is
/// deliberately dropped: only cycle and intra-cycle offset carry.
[[nodiscard]] constexpr uint32_t BaseTickFromCycleTimer(uint32_t cycleTimer) noexcept {
    const auto fields = ::ASFW::Timing::decodeCycleTimer(cycleTimer);
    return (fields.cycle % kCyclesPerSecond) * kTicksPerCycle +
           (fields.offset % kTicksPerCycle);
}

/// Decode the first data block's SPH as an offset from the packet's arrival time.
///
/// `payload` is the whole isochronous payload including the CIP header. Only the first
/// block is read: the replay cache stores one presentation offset per packet, and the
/// remaining blocks in a packet are evenly spaced by construction.
[[nodiscard]] inline MotuRxTiming DecodeRxTiming(std::span<const uint8_t> payload,
                                                 uint32_t dbs,
                                                 uint32_t packetCycleTimer,
                                                 uint32_t cipHeaderBytes = kCipHeaderBytes) noexcept {
    MotuRxTiming timing{};
    if (dbs == 0) {
        return timing;
    }
    const uint64_t blockBytes = static_cast<uint64_t>(dbs) * 4ULL;
    // A block must at least contain its SPH quadlet, and the payload must contain a
    // whole first block past the CIP header.
    if (blockBytes < 4ULL ||
        payload.size() < static_cast<uint64_t>(cipHeaderBytes) + blockBytes) {
        return timing;
    }

    const uint32_t sph = ReadSph(payload.subspan(cipHeaderBytes, 4));
    timing.firstBlockSph = sph;
    timing.presentationOffsetTicks = TickOffsetFromBase(sph, BaseTickFromCycleTimer(packetCycleTimer));
    timing.valid = true;
    return timing;
}

} // namespace ASFW::Encoding::Motu
