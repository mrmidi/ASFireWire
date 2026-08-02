// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// AudioTypes.hpp - Shared protocol-facing audio topology/runtime types

#pragma once

#include <cstdint>

namespace ASFW::Audio {

// Maximum isochronous streams per direction we support (DICE allows up to 4;
// the Venice F32 uses 2×16-channel streams per direction).
inline constexpr uint32_t kMaxAudioStreamsPerDirection = 4;

// Per-stream wire geometry for one isochronous stream within a direction.
// The aggregate device (what CoreAudio sees) is the sum across active streams,
// but the transport layer must drive each stream on its own iso channel.
struct AudioStreamWireInfo {
    static constexpr uint8_t kInvalidIsoChannel = 0xFF;

    uint8_t  isoChannel{kInvalidIsoChannel}; // assigned/discovered iso channel
    uint16_t pcmChannels{0};                 // PCM channels carried by this stream
    uint16_t am824Slots{0};                  // AM824 data-block slots (DBS) for this stream
    uint16_t midiPorts{0};                   // MIDI ports muxed into this stream
};

struct AudioStreamRuntimeCaps {
    static constexpr uint8_t kInvalidIsoChannel = 0xFF;

    // ---- Aggregate (CoreAudio / HAL view): summed across active streams ----
    // Host-facing channel counts (PCM only).
    uint32_t hostInputPcmChannels{0};   // Device -> host capture channels
    uint32_t hostOutputPcmChannels{0};  // Host -> device playback channels

    // Wire-slot counts (AM824 data block slots) when known.
    uint32_t deviceToHostAm824Slots{0}; // DICE TX stream slots (capture wire format)
    uint32_t hostToDeviceAm824Slots{0}; // DICE RX stream slots (playback wire format)

    uint32_t sampleRateHz{0};

    // First active DICE isochronous channel per direction (stream[0]).
    uint8_t deviceToHostIsoChannel{kInvalidIsoChannel}; // DICE TX / host IR
    uint8_t hostToDeviceIsoChannel{kInvalidIsoChannel}; // DICE RX / host IT

    // ---- Per-stream (wire view): drives transport + device register programming ----
    // Stream count comes from the DICE TX_NUMBER/RX_NUMBER registers, NOT the
    // count of currently-active streams — a freshly probed device may report
    // streams with iso=-1 that we still must arm.
    uint32_t deviceToHostStreamCount{0}; // # DICE TX streams (host IR)
    uint32_t hostToDeviceStreamCount{0}; // # DICE RX streams (host IT)
    AudioStreamWireInfo deviceToHostStreams[kMaxAudioStreamsPerDirection]{};
    AudioStreamWireInfo hostToDeviceStreams[kMaxAudioStreamsPerDirection]{};
};

struct AudioDuplexChannels {
    // Legacy single-channel accessors == stream[0] of each direction. Kept so
    // the single-stream host path (and existing call sites) compile unchanged.
    uint8_t deviceToHostIsoChannel{0};  // DICE TX / host IR (stream[0])
    uint8_t hostToDeviceIsoChannel{1};  // DICE RX / host IT (stream[0])

    // Per-stream iso channels the host assigns and writes into the device's
    // per-stream ISOC registers before GLOBAL_ENABLE. Counts default to 1 so an
    // unpopulated value behaves like the legacy single-stream config.
    //
    // INVARIANT: stream[0] of each direction is the legacy scalar field above;
    // these arrays carry the *additional* streams (index >= 1). Use the
    // CaptureChannel()/PlaybackChannel() accessors so single-stream call sites
    // that only populate the scalar fields stay byte-for-byte unchanged.
    uint32_t captureStreamCount{1};   // device TX streams (host IR)
    uint32_t playbackStreamCount{1};  // device RX streams (host IT)
    uint8_t captureIsoChannels[kMaxAudioStreamsPerDirection]{0};   // device TX -> host IR
    uint8_t playbackIsoChannels[kMaxAudioStreamsPerDirection]{1};  // host IT -> device RX

    // Iso channel for capture (device TX -> host IR) stream `i`.
    [[nodiscard]] uint8_t CaptureChannel(uint32_t i) const noexcept {
        return (i == 0) ? deviceToHostIsoChannel
                        : captureIsoChannels[i % kMaxAudioStreamsPerDirection];
    }
    // Iso channel for playback (host IT -> device RX) stream `i`.
    [[nodiscard]] uint8_t PlaybackChannel(uint32_t i) const noexcept {
        return (i == 0) ? hostToDeviceIsoChannel
                        : playbackIsoChannels[i % kMaxAudioStreamsPerDirection];
    }
};

} // namespace ASFW::Audio
