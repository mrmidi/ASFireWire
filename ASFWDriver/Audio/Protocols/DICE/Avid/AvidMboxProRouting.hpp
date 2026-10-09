// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// AvidMboxProRouting.hpp - TCAT EAP router program for the Avid Mbox Pro.
//
// The Mbox Pro powers up with no usable router program: nothing reaches the
// analog stage until a host writes one. Avid's control panel did this, so
// without it the device streams correctly and stays silent.
//
// This table is derived from the device's own factory configuration rather than
// from the Avid kext. The kext's `_sRoutingTable` was tried first and is wrong
// for our purposes: it routes playback to InS1 only and never writes InS0, which
// yields audio on headphone A and silence on every other output.
//
// The factory program was recovered by reading the EAP `currCfg` section, which
// holds the *active* configuration (the `router` section is only a staging
// buffer - reading it on a freshly attached device returns zero entries even
// though the device is working). currCfg contains three identical blocks, one
// per rate mode, at quadlets 0, 2048 and 4096; each begins with an entry count
// followed by the entries.
//
// Block map, confirmed on hardware (2026-09-17):
//
//   InS0 as destination = the six analog line outputs.
//   InS0 as source      = the analog inputs.
//   InS1 as destination = the headphone outputs.
//   AES  as destination = S/PDIF.
//
// Entries are (src << 8) | dst: the HIGH byte is where the signal comes from,
// the LOW byte is where it lands. Each byte is packed as (block << 4) | channel.
// This was read the other way round at first, which inverted the whole map and
// cost a long detour; the orientation is settled by playing audio and reading
// the peak section, where 0x2040 carries the music and its byte-swapped twin
// 0x4020 sits at zero. Destination and source use different block enums; ATX0
// (host playback, a source) and ARX0 (host capture, a destination) both encode
// as block 11.
//
// The commit opcode is kExecute | (Low|Middle|High) | kLoadRouter, i.e.
// 0x80070001. Committing only the low rate mode does not take effect. It appears
// in the kext as the constant 0x01000780 (little-endian of the wire value).
//
// 2026-10-09: line outs 3-4 silently carried the stereo monitor mix instead of
// playback channels 3-4. The router sent mixer outputs 0-1 to BOTH output
// pairs and parked outputs 2-3 - which already had correct, unity-gain
// coefficients for channels 3-4 - on MUTED. Fixed by routing outputs 2-3 to
// line outs 3-4 instead of duplicating 0-1 there; no mixer coefficient
// changed. Found and verified on a second Mbox Pro, on macOS Sequoia with a
// separate user-space driver built against this device's own register map.

#pragma once

#include <cstdint>

// Uses the shared startup-mixer cell type so the table needs no cast.
#include "../TCAT/DICETcatProtocol.hpp"

namespace ASFW::Audio::DICE::Avid::MboxProRouting {

// Captured from Avid's own driver running on macOS Mojave, with the line
// outputs and both headphones sounding at once, and replayed successfully here.
//
// The shape of it matters: **everything goes through the mixer**. The line
// outputs and S/PDIF are fed from mixer outputs, capture is taken from mixer
// outputs, and both serial port banks feed the mixer as sources. An earlier
// attempt routed playback straight to the outputs and capture straight from the
// serial ports, which made the two banks appear mutually exclusive - they are
// not, that was an artefact of bypassing the mixer.
//
// The headphones are fed from mixer outputs 8-13, not from block 3. Block 3
// appears only as the destination of InS1:0-1 and carries no monitor path; two
// earlier readings of it, first as a deliberate mute and then as the headphone
// A feed, were both wrong.
inline constexpr uint16_t kRouterEntries[] = {
    // Capture: the six analog inputs and S/PDIF go up to the host.
    0x40b0, 0x41b1, 0x42b2, 0x43b3, 0x44b4, 0x45b5,
    0x00b6, 0x01b7,
    // The same inputs also feed mixer inputs 0-5, and S/PDIF feeds 6-7, so the
    // monitor mix can carry them without the host being involved.
    0x4020, 0x4121, 0x4222, 0x4323, 0x4424, 0x4525,
    0x0026, 0x0127,
    // Host playback lands on mixer inputs 8-15. These are the strips the panel
    // calls the software returns.
    0xb028, 0xb129, 0xb22a, 0xb32b, 0xb42c, 0xb52d, 0xb62e, 0xb72f,
    // InS1:0-1 on to block 3.
    0x5030, 0x5131,
    // Mixer outputs 0-1 are the stereo monitor mix, to line outs 1-2. Outputs
    // 2-3 carry playback channels 3-4 independently, to line outs 3-4; they
    // are NOT a duplicate of 0-1. The earlier table routed the monitor mix to
    // both pairs and parked 2-3 on MUTED below, so playback channels 3-4 of
    // the host never reached an output even though their mixer coefficients
    // (indices 46 and 65, "straight passes at unity" further down) were
    // already correct and simply unused. Outputs 4-5 feed line outs 5-6.
    0x2040, 0x2141, 0x2242, 0x2343, 0x2444, 0x2545,
    // Mixer outputs 6-7 leave as S/PDIF.
    0x2600, 0x2701,
    // Mixer outputs 8-13 feed the headphones. With the coefficients below,
    // outputs 8-9 carry playback 1-2 and 10-11 carry playback 3-4, which is
    // exactly what Avid's manual says the two headphone jacks monitor.
    0x2850, 0x2951, 0x2a52, 0x2b53, 0x2c54, 0x2d55,
    // Mixer outputs 14 and 15 go nowhere; parked on MUTED.
    0x2ef0, 0x2ff1,
};
inline constexpr uint32_t kRouterEntryCount =
    static_cast<uint32_t>(sizeof(kRouterEntries) / sizeof(kRouterEntries[0]));

// The mixer coefficients that came with that capture. Without them the MIXo
// path carries silence and nothing reaches the outputs, which is exactly how
// an earlier experiment was misread as "the mixer path does not work".
// Index is output * 18 + input; unity is 0x4000.
using MixerCoefficient = TCAT::DiceStartupMixerCell;
inline constexpr MixerCoefficient kStartupMixerCoefficients[] = {
    // Outputs 0-1: the stereo mix, inputs 8-16 at assorted gains.
    {8, 0x0abb}, {9, 0x12b7}, {10, 0x12b7}, {11, 0x12b7},
    {12, 0x12b7}, {13, 0x12b7}, {14, 0x10d4}, {15, 0x10d4},
    {16, 0x19f2},
    {26, 0x0abb}, {27, 0x12b7}, {28, 0x12b7}, {29, 0x12b7},
    {30, 0x12b7}, {31, 0x12b7}, {32, 0x10d4}, {33, 0x10d4},
    {34, 0x19f2},
    // Outputs 2-7: straight passes at unity.
    {46, 0x4000}, {65, 0x4000}, {84, 0x4000}, {103, 0x4000},
    {122, 0x4000}, {141, 0x4000},
    // Outputs 8-11: the capture sends, also unity.
    {152, 0x4000}, {171, 0x4000}, {190, 0x4000}, {209, 0x4000},
};
inline constexpr uint32_t kStartupMixerCoefficientCount =
    static_cast<uint32_t>(sizeof(kStartupMixerCoefficients) /
                          sizeof(kStartupMixerCoefficients[0]));

} // namespace ASFW::Audio::DICE::Avid::MboxProRouting
