// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// DuplexIRMAdvisory.hpp - when a failed IRM reservation may be treated as advisory
//
// An isochronous reservation answers one of two different questions depending on
// the device. For a CMP device the IRM *chooses* the channel, and the answer is
// the reservation itself - without an IRM there is nothing to fall back to. For a
// register-protocol device (DICE family, MOTU) the channel is already decided by
// the device before we get here, and the IRM transaction only publishes that
// claim to the bus. On a bus with no IRM responder there is nobody to publish it
// to and nobody to collide with, so the start can proceed.
//
// This matters because ASFW required the reservation unconditionally, which on a
// bare two-node MacBook<->MOTU bus (no node is IRM) failed every StartIO with
// kIOReturnNoDevice even though the device-facing register choreography had
// already succeeded. It is also a parity gap: a bus capture of the official
// MOTU driver on OS X 10.11 shows no IRM or bandwidth allocation at all on an
// ordinary playback start.

#pragma once

#include "../AudioTypes.hpp"

#include <DriverKit/IOReturn.h>

#include <cstdint>

namespace ASFW::Audio::Backends {

// Only these two statuses mean "there is nobody on this bus to negotiate with":
// IRMTypes.hpp documents NotFound as "No IRM node on bus, or CSR access returns
// address_error", and Timeout as a designated IRM that never answered.
//
// Everything else stays fatal on purpose. NoResources is a genuine conflict -
// another node really holds that channel or the bandwidth - and proceeding would
// put us on the wire against a live claimant. GenerationMismatch means the bus
// reset out from under us and the restart epoch is already stale.
[[nodiscard]] constexpr bool IsAbsentIrmStatus(kern_return_t status) noexcept {
    return status == kIOReturnNoDevice || status == kIOReturnTimeout;
}

// A device-assigned channel is only recoverable from a mask that names exactly
// one channel. This is re-checked independently of the profile flag so that a
// wider mask with the flag mistakenly set cannot silently pick an arbitrary
// channel - it fails closed instead.
[[nodiscard]] constexpr uint8_t SoleChannelInMask(uint64_t allowedChannels) noexcept {
    if (allowedChannels == 0 || (allowedChannels & (allowedChannels - 1)) != 0) {
        return AudioStreamWireInfo::kInvalidIsoChannel;
    }
    for (uint8_t channel = 0; channel < 64; ++channel) {
        if ((allowedChannels & (uint64_t{1} << channel)) != 0) {
            return channel;
        }
    }
    return AudioStreamWireInfo::kInvalidIsoChannel;
}

// kInvalidIsoChannel means "no advisory fallback applies - propagate the original
// reservation failure". A valid channel means the caller may proceed on it
// without holding a bus reservation.
[[nodiscard]] constexpr uint8_t AdvisoryFallbackChannel(kern_return_t reservationStatus,
                                                        uint64_t allowedChannels,
                                                        bool deviceOwnsChannel) noexcept {
    if (!deviceOwnsChannel || !IsAbsentIrmStatus(reservationStatus)) {
        return AudioStreamWireInfo::kInvalidIsoChannel;
    }
    return SoleChannelInMask(allowedChannels);
}

} // namespace ASFW::Audio::Backends
