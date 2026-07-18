// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// Phase88MixerData.hpp - Static Function Block mixer map for TerraTec PHASE 88 Rack FW.
//
// PHASE 88 ships with its internal hardware mixer muted and at minimum volume.
// After signal format programming, the stream playback path (FB 0x07) and mixer
// output path (FB 0x00/0x01) must be explicitly unmuted and set to maximum gain,
// or streaming is silent despite working CIP DMA.
//
// This is an ASFW-specific workaround not present in Linux bebob_terratec.c
// (which only reads clock selectors 8/9; never programs the mixer) or FFADO
// terratec_device.cpp (clock source selection only). Cross-validated against
// hardware: TERRATEC_TOPOLOGY_RESEARCH.md (2026-07-16).
//
// FB map (from device PUBs and FFADO libffado-2.5.0/src/bebob/terratec/):
//   0x06 Selector: mixer destination (0x01 = analog-output-1/2)
//   0x07 Selector: mixer stream source (0x01 = stream-input-1/2)
//   0x07 Feature:  stream playback mute/volume (ch1=L, ch2=R)
//   0x00 Feature:  mixer output 0 mute/volume (physical output 1)
//   0x01 Feature:  mixer output 1 mute/volume (physical output 2)

#pragma once

#include "BeBoBMixerMap.hpp"

#include <array>
#include <cstdint>

namespace ASFW::Audio::BeBoB {

inline constexpr std::array kPhase88Selectors{
    SelectorRoute{0x06, 0x01},  // Mixer Destination = analog-output-1/2
    SelectorRoute{0x07, 0x01},  // Mixer Stream Source = stream-input-1/2
};

inline constexpr std::array kPhase88Mutes{
    ChannelMute{0x07, 1, true},   // Unmute Stream Playback Left
    ChannelMute{0x07, 2, true},   // Unmute Stream Playback Right
    ChannelMute{0x00, 1, true},   // Unmute Mixer Output Left
    ChannelMute{0x01, 1, true},   // Unmute Mixer Output Right
};

inline constexpr std::array kPhase88Volumes{
    ChannelVolume{0x07, 1, 0x0000},  // Max Vol Stream Playback Left
    ChannelVolume{0x07, 2, 0x0000},  // Max Vol Stream Playback Right
    ChannelVolume{0x00, 1, 0x0000},  // Max Vol Mixer Output Left
    ChannelVolume{0x01, 1, 0x0000},  // Max Vol Mixer Output Right
};

inline constexpr MixerMap kPhase88MixerMap{
    .selectors = kPhase88Selectors,
    .mutes = kPhase88Mutes,
    .volumes = kPhase88Volumes,
};

} // namespace ASFW::Audio::BeBoB
