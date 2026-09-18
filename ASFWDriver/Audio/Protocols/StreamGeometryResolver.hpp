// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// StreamGeometryResolver.hpp — one source of truth for per-stream wire geometry.
//
// Two independent descriptions of the same isochronous streams exist in this
// driver:
//
//   - The DEVICE's own DICE TX_/RX_ registers, read at bring-up into
//     AudioStreamRuntimeCaps::{deviceToHost,hostToDevice}Streams[].
//   - The PROFILE's compiled-in constants, reached through
//     IAudioStreamProfile::BuildTxStreamConfig(index, ...).
//
// The transport reserved isoch bandwidth from the first while the packetizer
// framed CIP from the second, and nothing compared them. A disagreement
// therefore produced correctly-reserved bandwidth carrying wrongly-framed
// packets — whose symptom is silence with no error logged anywhere. The hazard
// is spelled out in PreSonusStudioLive2442Profile.cpp's header comment; this
// header is what closes it.
//
// The precedence rule is the device's, for a reason that is not a preference:
// PreSonus's own driver carries no host-side geometry constants at all.
// PaeFireStudio.kext 4.2.1 reads RX_NUMBER_AUDIO per stream in PopulateRxStruct
// and frames from exactly that, for all fourteen devices it supports, with no
// per-model branch (RE'd 2026-09-18). So the device is authoritative, the
// profile is an expectation we check it against, and a disagreement is a defect
// in the profile that must be loud rather than silent.
//
// Deliberately free of DriverKit and of IAudioStreamProfile: it takes plain
// scalars so the host test suite can reach every branch, including the
// asymmetric multi-stream cases no bench device can produce.

#pragma once

#include <cstdint>

namespace ASFW::Audio {

// One isochronous stream's wire shape. pcmChannels == 0 means "not stated" —
// the enumeration carries the stream count, so a described stream always has at
// least one PCM channel.
struct WireStreamGeometry {
    uint16_t pcmChannels{0};
    uint16_t am824Slots{0};
    uint16_t midiPorts{0};

    [[nodiscard]] constexpr bool IsStated() const noexcept { return pcmChannels != 0; }

    friend constexpr bool operator==(const WireStreamGeometry&,
                                     const WireStreamGeometry&) noexcept = default;
};

enum class StreamGeometrySource : uint8_t {
    kProfile = 0,  ///< The device stated nothing for this stream.
    kDevice  = 1,  ///< The device stated it; the profile agreed or was silent.
};

struct StreamGeometryDecision {
    WireStreamGeometry geometry{};
    StreamGeometrySource source{StreamGeometrySource::kProfile};

    /// Both sides described this stream and they do not match. The geometry
    /// field still carries the device's value, but callers must refuse to
    /// start: whichever side is wrong, framing and bandwidth would disagree.
    bool disagrees{false};

    WireStreamGeometry deviceStated{};
    WireStreamGeometry profileStated{};

    [[nodiscard]] constexpr bool Usable() const noexcept {
        return geometry.IsStated() && !disagrees;
    }
};

struct StreamCountDecision {
    uint32_t count{0};
    StreamGeometrySource source{StreamGeometrySource::kProfile};
    bool disagrees{false};
    uint32_t deviceStated{0};
    uint32_t profileStated{0};
};

/// Resolve how many streams a direction carries.
///
/// This is a second, independent divergence from the per-stream one below: the
/// HAL side arms profile->TxStreamCount() streams while the transport side arms
/// caps.hostToDeviceStreamCount. When those differ the transport reserves
/// bandwidth for streams the packetizer never feeds (or the reverse), which
/// again fails silently.
///
/// `maxStreams` is the host's hard array bound (kMaxAudioStreamsPerDirection).
/// Both vendor drivers enforce their own: PaeFireStudio and Saffire each reject
/// RX_NUMBER > 4 outright rather than clamping, so a device claiming more is
/// refused, not truncated.
[[nodiscard]] constexpr StreamCountDecision
ResolveStreamCount(uint32_t deviceStated,
                   uint32_t profileStated,
                   uint32_t maxStreams) noexcept {
    StreamCountDecision decision{};
    decision.deviceStated = deviceStated;
    decision.profileStated = profileStated;

    if (deviceStated == 0) {
        decision.count = (profileStated <= maxStreams) ? profileStated : maxStreams;
        decision.source = StreamGeometrySource::kProfile;
        return decision;
    }

    decision.source = StreamGeometrySource::kDevice;
    if (deviceStated > maxStreams) {
        // Refuse rather than truncate: silently dropping a stream the device
        // will transmit on is exactly the silent-wrong-geometry class this
        // header exists to end.
        decision.count = 0;
        decision.disagrees = true;
        return decision;
    }

    decision.count = deviceStated;
    decision.disagrees = profileStated != 0 && profileStated != deviceStated;
    return decision;
}

/// Resolve one stream's framing geometry. Pure; no fallback is invented — when
/// neither side states a geometry the result is simply unusable, which is a
/// louder and more honest outcome than substituting a plausible number.
[[nodiscard]] constexpr StreamGeometryDecision
ResolveStreamGeometry(WireStreamGeometry deviceStated,
                      WireStreamGeometry profileStated) noexcept {
    StreamGeometryDecision decision{};
    decision.deviceStated = deviceStated;
    decision.profileStated = profileStated;

    if (!deviceStated.IsStated()) {
        // No device geometry: the profile is all we have. That is the normal
        // case for families whose protocols publish only aggregate caps, so it
        // is not by itself an error.
        decision.geometry = profileStated;
        decision.source = StreamGeometrySource::kProfile;
        return decision;
    }

    decision.geometry = deviceStated;
    decision.source = StreamGeometrySource::kDevice;

    if (!profileStated.IsStated()) {
        return decision;
    }

    // Channel count decides how many PCM slots the packetizer writes; the AM824
    // slot count decides the data-block size on the wire. Either one differing
    // changes the packet the device receives, so either one disagreeing is
    // disqualifying. MIDI ports are compared only through am824Slots, which
    // already includes them.
    const bool channelsDiffer = deviceStated.pcmChannels != profileStated.pcmChannels;
    const bool slotsDiffer = deviceStated.am824Slots != 0 && profileStated.am824Slots != 0 &&
                             deviceStated.am824Slots != profileStated.am824Slots;
    decision.disagrees = channelsDiffer || slotsDiffer;
    return decision;
}

} // namespace ASFW::Audio
