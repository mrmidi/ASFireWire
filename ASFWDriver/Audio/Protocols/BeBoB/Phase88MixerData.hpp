// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// Phase88MixerData.hpp - Static Function Block mixer map for TerraTec PHASE 88 Rack FW.
//
// PHASE 88 ships with its internal hardware mixer muted and at minimum volume.
// After signal format programming, the stream playback path (FB 0x07) and mixer
// output path (FB 0x01) is explicitly unmuted; only WavePlay input gain is set,
// or streaming is silent despite working CIP DMA.
//
// This is an ASFW-specific workaround not present in Linux bebob_terratec.c
// (which only reads clock selectors 8/9; never programs the mixer) or FFADO
// terratec_device.cpp (clock source selection only). Cross-validated against
// hardware: TERRATEC_TOPOLOGY_RESEARCH.md (2026-07-16).
//
// FB map (FFADO support/mixer-qt4/ffado/mixer/phase88control.py:42-64, ui labels
// in phase88.ui; confirmed on hardware 2026-09-27):
//   0x06 Selector: mixer destination "Out Assign" (0x01 = Line Out 1/2)
//   0x07 Selector: mixer stream source (0x01 = stream-input-1/2)
//   0x07 Feature:  "WavePlay", stream playback into the mixer (ch1=L, ch2=R)
//   0x01 Feature:  mixer Master (ch1=L, ch2=R)
// Feature block 0 does not exist: STATUS answers NOT IMPLEMENTED.
//
// Volume is a signed 16-bit value in 1/256 dB. The Phase 88 reports -100 dB
// (0x9C00) to 0 dB (0x0000) in 1 dB steps for the Master (STATUS min/max/
// resolution, 2026-09-27). The unit has no output volume knob, so the Master
// retains its existing channel levels; the discovered channel-0 master is
// published to Core Audio instead of installing a listening-level preset.

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
    ChannelMute{0x01, 1, true},   // Unmute Master Left
    ChannelMute{0x01, 2, true},   // Unmute Master Right
};

inline constexpr std::array kPhase88Volumes{
    ChannelVolume{0x07, 1, 0x0000},                // Stream Playback Left, 0 dB
    ChannelVolume{0x07, 2, 0x0000},                // Stream Playback Right, 0 dB
};

inline constexpr MixerMap kPhase88MixerMap{
    .selectors = kPhase88Selectors,
    .mutes = kPhase88Mutes,
    .volumes = kPhase88Volumes,
};

} // namespace ASFW::Audio::BeBoB
