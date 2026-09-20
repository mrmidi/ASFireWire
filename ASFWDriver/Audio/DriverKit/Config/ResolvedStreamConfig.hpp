// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// ResolvedStreamConfig.hpp — turn resolved per-stream geometry into wire config.
//
// The publishing side reads the device's TX/RX registers, validates them
// against the profile and carries the result across the nub
// (Audio/Model/AudioPropertyKeys.hpp). This is where the audio side turns that
// into the AudioStreamConfig its packetizer actually uses.
//
// The split of authority is the point:
//
//   - The DEVICE owns how many streams there are, how many PCM channels and
//     AM824 slots each carries, and where each one starts in the host buffer.
//   - The PROFILE owns the framing constants the DICE registers do not hold:
//     fdf, fmt, frames per data packet, stream mode, sid.
//
// Before this existed the profile owned both, so an asymmetric device was
// framed from constants describing a different one. The recorded Midas Venice
// F24 carries 16 + 8 playback channels while its profile describes an F32 at
// 16 + 16, which put 16 channels and DBS 16 into a stream the device frames
// with 8 slots — correctly reserved bandwidth carrying wrongly framed packets,
// whose symptom is silence with nothing logged.
//
// Deliberately free of DriverKit so the host suite can drive every branch;
// ASFWAudioDevice::StartIO is the only production caller.

#pragma once

#include "AudioDriverConfig.hpp"
#include "AudioStreamProfile.hpp"

#include <cstdint>

namespace ASFW::Isoch::Audio {

/// How many playback streams to arm.
///
/// The device's resolved count wins. A zero count means the publisher carried
/// no geometry — non-DICE backends, or a build talking to an older publisher —
/// and the profile's own count is then the only answer available. That is
/// correct for a uniform device and wrong for an asymmetric one, so callers
/// should say so in the log rather than pass it over silently.
[[nodiscard]] inline constexpr uint32_t
ResolvedPlaybackStreamCount(uint32_t profileStreamCount,
                            uint32_t resolvedStreamCount) noexcept {
    return resolvedStreamCount != 0 ? resolvedStreamCount : profileStreamCount;
}

/// Build one playback stream's wire config.
///
/// The base config is always the profile's stream 0, never stream `index`: the
/// resolved stream count belongs to the device, and asking a profile about a
/// stream it does not know exists would fail for exactly the devices this
/// exists to support (a Saffire profile declares one playback stream; a device
/// may report two).
///
/// With no resolved geometry for `index`, this degrades to the profile's own
/// indexed accessor — the behaviour that predates the nub carrying geometry.
[[nodiscard]] inline bool
BuildResolvedTxStreamConfig(const IAudioStreamProfile& profile,
                            const ParsedWireStream* resolvedStreams,
                            uint32_t resolvedStreamCount,
                            uint32_t index,
                            AudioStreamConfig& outConfig) noexcept {
    if (resolvedStreams == nullptr || index >= resolvedStreamCount) {
        return profile.BuildTxStreamConfig(index, outConfig);
    }
    if (!profile.BuildTxStreamConfig(0, outConfig)) {
        return false;
    }

    const ParsedWireStream& wire = resolvedStreams[index];
    outConfig.pcmChannels = static_cast<uint8_t>(wire.pcmChannels);
    outConfig.midiSlots = static_cast<uint8_t>(wire.midiPorts);
    outConfig.dbs = static_cast<uint8_t>(wire.am824Slots);
    // The publisher computed this as the running sum of preceding stream
    // widths. It is NOT index * width-of-stream-0: those agree only while every
    // stream is stream 0's width, and the vendor drivers carry the same running
    // base (AlesisFirewireAudioEngine::CreateStreams advances it per stream).
    outConfig.sourceChannelOffset = static_cast<uint8_t>(wire.channelOffset);
    return true;
}

/// Isochronous packet payload size for a stream: the CIP header plus this
/// stream's data blocks. Shared with StartIO so the buffer it allocates and the
/// geometry it frames from cannot disagree — sizing from one description while
/// framing from another is the defect this whole path removes.
[[nodiscard]] inline constexpr uint32_t
TxPacketBytesForStreamConfig(const AudioStreamConfig& config) noexcept {
    return 8u + static_cast<uint32_t>(config.framesPerDataPacket) *
                    static_cast<uint32_t>(config.dbs) * 4u;
}

} // namespace ASFW::Isoch::Audio
