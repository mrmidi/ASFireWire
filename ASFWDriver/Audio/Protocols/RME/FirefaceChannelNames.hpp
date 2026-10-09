// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
#pragma once

#include <array>

namespace ASFW::Audio::RME {

// Channel names at 48 kHz with every channel sent, in wire order. Taken from
// the RME 3.41 driver's tables (gChannelNames_FF400_in/out at 0x8020/0x8140,
// gChannelNames_FF800_in/out at 0x8260/0x8420); FFADO addDirPorts orders the
// ports the same way: analog, phones (playback only), S/PDIF, ADAT
// (rme_avdevice.cpp:1063-1140).
inline constexpr std::array<const char*, 18> kFF400InputNames{
    "Mic/Line 1", "Mic/Line 2", "Inst/Line 3", "Inst/Line 4",
    "Analog 5", "Analog 6", "Analog 7", "Analog 8",
    "SPDIF L", "SPDIF R",
    "ADAT 1", "ADAT 2", "ADAT 3", "ADAT 4", "ADAT 5", "ADAT 6", "ADAT 7", "ADAT 8"};

inline constexpr std::array<const char*, 18> kFF400OutputNames{
    "Analog 1", "Analog 2", "Analog 3", "Analog 4", "Analog 5", "Analog 6",
    "Phones 7", "Phones 8",
    "SPDIF L", "SPDIF R",
    "ADAT 1", "ADAT 2", "ADAT 3", "ADAT 4", "ADAT 5", "ADAT 6", "ADAT 7", "ADAT 8"};

inline constexpr std::array<const char*, 28> kFF800InputNames{
    "Analog 1", "Analog 2", "Analog 3", "Analog 4",
    "Analog 5", "Analog 6", "Analog 7", "Analog 8",
    "Mic 9", "Mic 10",
    "SPDIF L", "SPDIF R",
    "ADAT 1", "ADAT 2", "ADAT 3", "ADAT 4", "ADAT 5", "ADAT 6", "ADAT 7", "ADAT 8",
    "ADAT 9", "ADAT 10", "ADAT 11", "ADAT 12", "ADAT 13", "ADAT 14", "ADAT 15", "ADAT 16"};

inline constexpr std::array<const char*, 28> kFF800OutputNames{
    "Analog 1", "Analog 2", "Analog 3", "Analog 4",
    "Analog 5", "Analog 6", "Analog 7", "Analog 8",
    "Phones 9", "Phones 10",
    "SPDIF L", "SPDIF R",
    "ADAT 1", "ADAT 2", "ADAT 3", "ADAT 4", "ADAT 5", "ADAT 6", "ADAT 7", "ADAT 8",
    "ADAT 9", "ADAT 10", "ADAT 11", "ADAT 12", "ADAT 13", "ADAT 14", "ADAT 15", "ADAT 16"};

} // namespace ASFW::Audio::RME
