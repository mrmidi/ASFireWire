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

} // namespace ASFW::Testing::DICE::SyntheticDiceImages
