// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// SyntheticDiceImages.hpp — device images derived from a recorded dump for a
// case no dump shows. Each says what it changes and why.

#pragma once

#include "DiceDeviceImage.hpp"

namespace ASFW::Testing::DICE::SyntheticDiceImages {

// The recorded MultiMix (a 12-input unit, RX_NUMBER = 1) announcing a second
// playback stream: the case libffado clamps to one for Alesis models 0 and 1
// ("announces two receive transmitters, but only has one",
// dice_avdevice.cpp:1686-1700). Alesis's own kext allocates a stream for every
// one the register reports (AlesisFirewire 3.5.6 AllocateStreams). The dump
// does not show the unused RX blocks, so stream 1 copies stream 0.
constexpr DiceDeviceImage MakeMultimixTwoPlayback() {
    DiceDeviceImage image = DiceDeviceImages::kMultimix;
    image.key = "multimix-two-playback";
    image.rxCount = 2;
    image.rx[1] = image.rx[0];
    return image;
}
inline constexpr DiceDeviceImage kMultimixTwoPlayback = MakeMultimixTwoPlayback();

// The recorded FireStudio 2626 (no EAP) with stream registers that follow the
// rate mode, as a DICE's TX/RX registers describe its current mode only. No
// dump shows the 2626 at 2x: the 2x layout here (16+MIDI and 2 each way) is
// invented to differ from 1x, so a test can tell which mode it read.
constexpr DiceDeviceImage MakeFirestudio2626RegistersFollowMode() {
    DiceDeviceImage image = DiceDeviceImages::kFirestudio2626;
    image.key = "firestudio-2626-registers-follow-mode";
    image.hasRateModeFormats = true;
    for (auto* direction : {&image.rateModes[0].tx, &image.rateModes[0].rx}) {
        direction->count = 2;
        direction->streams[0] = {.pcm = 16, .midi = 1};
        direction->streams[1] = {.pcm = 10, .midi = 0};
    }
    for (auto* direction : {&image.rateModes[1].tx, &image.rateModes[1].rx}) {
        direction->count = 2;
        direction->streams[0] = {.pcm = 16, .midi = 1};
        direction->streams[1] = {.pcm = 2, .midi = 0};
    }
    image.rateModes[2] = image.rateModes[1];
    return image;
}
inline constexpr DiceDeviceImage kFirestudio2626RegistersFollowMode = MakeFirestudio2626RegistersFollowMode();

} // namespace ASFW::Testing::DICE::SyntheticDiceImages
