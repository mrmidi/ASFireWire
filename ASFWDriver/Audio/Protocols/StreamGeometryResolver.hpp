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

// ---------------------------------------------------------------------------
// The resolved object (Stage 4): one per-direction, per-stream answer that
// every consumer reads, instead of bandwidth reservation reading the device
// and CIP framing reading the profile.
//
// NOT a forward port. The `midi` branch has no counterpart: its
// AudioGeometry{Policy,Resolver,Report} trio resolves *timing* geometry --
// frames-per-packet, safety offsets and reported latency as functions of
// sample rate -- not the per-stream wire shape, and it predates the
// device-vs-profile conflict settled here (which arrived with #129 on main).
// Recorded so nobody goes looking for a midi reference that does not exist.
//
// Directions are named for the HOST, not the device, because DICE register
// names invert: DICE TX is what the device transmits, i.e. host capture, and
// DICE RX is host playback. Mixing those up is the standing trap in this area.
// ---------------------------------------------------------------------------

/// Must equal kMaxAudioStreamsPerDirection (Audio/Protocols/AudioTypes.hpp).
/// Duplicated rather than included so this header stays a pure, plain-scalar
/// unit the host suite can compile on its own; the backend static_asserts the
/// two agree.
inline constexpr uint32_t kMaxResolvedStreams = 4;

struct ResolvedDirectionGeometry {
    StreamCountDecision count{};
    StreamGeometryDecision streams[kMaxResolvedStreams]{};

    /// How many streams this direction carries, after resolution.
    [[nodiscard]] constexpr uint32_t StreamCount() const noexcept { return count.count; }

    /// Sum of PCM channels across the streams this direction actually carries.
    /// This is the number the HAL presents, and it is NOT streams[0] times the
    /// stream count: the recorded Venice F24 carries 16 + 8, so a consumer that
    /// reads stream 0 and multiplies gets 32 where the device means 24.
    [[nodiscard]] constexpr uint32_t TotalPcmChannels() const noexcept {
        uint32_t total = 0;
        for (uint32_t i = 0; i < count.count && i < kMaxResolvedStreams; ++i) {
            total += streams[i].geometry.pcmChannels;
        }
        return total;
    }

    /// Index of the first stream whose two descriptions disagree, or
    /// kMaxResolvedStreams when none do. Named so the refusal can say which.
    [[nodiscard]] constexpr uint32_t FirstDisagreeingStream() const noexcept {
        for (uint32_t i = 0; i < count.count && i < kMaxResolvedStreams; ++i) {
            if (streams[i].disagrees) {
                return i;
            }
        }
        return kMaxResolvedStreams;
    }

    /// Every stream this direction carries is described and agreed. A direction
    /// carrying zero streams is usable: half-duplex devices exist (Weiss is
    /// TX-only), and refusing them here would be a regression.
    [[nodiscard]] constexpr bool Usable() const noexcept {
        if (count.disagrees) {
            return false;
        }
        for (uint32_t i = 0; i < count.count && i < kMaxResolvedStreams; ++i) {
            if (!streams[i].Usable()) {
                return false;
            }
        }
        return true;
    }
};

// A profile constant is only an input to resolution when the profile is
// ASSERTING it. A seeded constant -- one invented so the endpoint has plausible
// numbers before the device is read -- states nothing, and these two turn that
// into the "unstated" the functions above already handle.
//
// Without this, every unverified constant became a conflict with the device
// that actually knows, and the endpoint was refused. That is not hypothetical:
// a contributed Alesis MultiMix dump reports two capture streams of 12 + 2
// where the profile seeds one of 16, and the MultiMix 8/12/16 all publish the
// same vendor/model so no constant could have been right for all three.
//
// Taken as a bool rather than the profile's enum so this header stays a plain
// scalar unit; the caller maps its own authority type onto it.
[[nodiscard]] constexpr uint32_t
ProfileStatedStreamCount(bool asserted, uint32_t count) noexcept {
    return asserted ? count : 0U;
}

[[nodiscard]] constexpr WireStreamGeometry
ProfileStatedGeometry(bool asserted, WireStreamGeometry geometry) noexcept {
    return asserted ? geometry : WireStreamGeometry{};
}

/// Resolve one direction end to end: the stream count, then every stream in
/// the union of what each side describes, so a stream only one side knows
/// about still reaches the decision instead of being skipped.
///
/// Pure and templated on the accessors so the host suite can drive it with
/// plain lambdas; the caller does the logging.
template <typename DeviceAccessor, typename ProfileAccessor>
[[nodiscard]] constexpr ResolvedDirectionGeometry ResolveDirectionGeometry(
    uint32_t deviceStreamCount,
    uint32_t profileStreamCount,
    DeviceAccessor&& fromDevice,
    ProfileAccessor&& fromProfile) noexcept {
    ResolvedDirectionGeometry resolved{};
    resolved.count = ResolveStreamCount(deviceStreamCount, profileStreamCount,
                                        kMaxResolvedStreams);

    const uint32_t deviceStreams =
        (deviceStreamCount > kMaxResolvedStreams) ? kMaxResolvedStreams : deviceStreamCount;
    const uint32_t profileStreams =
        (profileStreamCount > kMaxResolvedStreams) ? kMaxResolvedStreams : profileStreamCount;
    const uint32_t walk = (deviceStreams > profileStreams) ? deviceStreams : profileStreams;

    for (uint32_t i = 0; i < walk && i < kMaxResolvedStreams; ++i) {
        resolved.streams[i] = ResolveStreamGeometry(fromDevice(i), fromProfile(i));
    }
    return resolved;
}

struct ResolvedDeviceGeometry {
    ResolvedDirectionGeometry capture{};   ///< device -> host (DICE TX)
    ResolvedDirectionGeometry playback{};  ///< host -> device (DICE RX)

    [[nodiscard]] constexpr bool Usable() const noexcept {
        return capture.Usable() && playback.Usable();
    }
};

} // namespace ASFW::Audio
