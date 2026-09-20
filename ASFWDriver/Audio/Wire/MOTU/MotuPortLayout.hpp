// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// MotuPortLayout.hpp - Which physical port each MOTU v2 PCM chunk carries, and the order
// CoreAudio sees them in.
//
// The wire order is the device's, not the user's: an UltraLite's first playback chunks
// are its headphone pair and its first capture chunks are the CueMix return, so a host
// that simply maps channel N to chunk N sends the Mac's default stereo pair to the
// headphones and records the mixer instead of the microphones. A port map fixes the
// host-side order without touching the wire -- FFADO does the same thing with its
// port_order field (motu_avdevice.cpp:1887-1900), creating the UltraLite's Main pair
// first.
//
// Chunk positions come from FFADO's port-group tables, whose packet offsets are assigned
// in array order, three bytes per channel from byte 10, with padding groups consuming
// space but creating no port (motu_avdevice.cpp:1839-1862):
//   - 828mk2:    PortGroups_828MKII,   motu_avdevice.cpp:111-121
//   - UltraLite: PortGroups_ULTRALITE, motu_avdevice.cpp:135-145
// Linux sound/firewire/motu carries the same chunk counts (motu-protocol-v2.c:274-310) but
// does not name ports, so FFADO is the only reference for which chunk is which jack.

#pragma once

#include <cstdint>
#include <span>

namespace ASFW::Encoding::Motu {

/// One physical port: the name CoreAudio shows and the PCM chunk that carries it.
struct MotuPort final {
    const char* name;
    uint8_t chunk;
};

/// Host channel order for one direction: host channel h carries `ports[h]`. Empty means
/// wire order. Channels past the table carry the chunk of the same index, so optical
/// extras (which follow every fixed chunk) need no entries.
using MotuPortMap = std::span<const MotuPort>;

/// Wire geometry plus host channel order for one MOTU stream direction.
struct MotuStreamLayout final {
    /// PCM chunks per data block, as resolved by MotuV2Protocol::PrepareDuplex.
    uint32_t pcmChunks{0};
    MotuPortMap ports{};
};

/// The map to apply for a stream of `pcmChunks` chunks. A table wider than the stream
/// (a rate mode that drops ports) cannot be a permutation of it, so it falls back to wire
/// order rather than leave chunks unwritten.
[[nodiscard]] constexpr MotuPortMap EffectivePortMap(MotuPortMap ports,
                                                     uint32_t pcmChunks) noexcept {
    return ports.size() <= pcmChunks ? ports : MotuPortMap{};
}

/// Chunk carrying host channel `hostChannel` under an effective map.
[[nodiscard]] constexpr uint32_t ChunkForHostChannel(MotuPortMap ports,
                                                     uint32_t hostChannel) noexcept {
    return hostChannel < ports.size() ? ports[hostChannel].chunk : hostChannel;
}

/// True when `ports` names every chunk in [0, size) exactly once, which is what makes the
/// identity tail in ChunkForHostChannel safe.
[[nodiscard]] constexpr bool IsChunkPermutation(MotuPortMap ports) noexcept {
    for (uint32_t chunk = 0; chunk < ports.size(); ++chunk) {
        uint32_t hits = 0;
        for (const MotuPort& port : ports) {
            hits += (port.chunk == chunk) ? 1U : 0U;
        }
        if (hits != 1) {
            return false;
        }
    }
    return true;
}

// Playback, shared by both models: chunks 0-1 Phones, 2-9 Analog 1-8, 10-11 Main,
// 12-13 S/PDIF. The UltraLite splits Analog into 1-2 and 3-8 groups, which lands them on
// the same chunks. Main comes first so the default stereo pair reaches the main outs.
inline constexpr MotuPort kV2Playback[] = {
    {"Main L", 10},   {"Main R", 11},   {"Analog 1", 2},  {"Analog 2", 3},
    {"Analog 3", 4},  {"Analog 4", 5},  {"Analog 5", 6},  {"Analog 6", 7},
    {"Analog 7", 8},  {"Analog 8", 9},  {"S/PDIF 1", 12}, {"S/PDIF 2", 13},
    {"Phones L", 0},  {"Phones R", 1},
};

// UltraLite capture: chunks 0-1 Mix return, 2-3 Mic 1-2, 4-9 Analog 3-8, 10-11 S/PDIF,
// 12-13 padding. The front-panel mic inputs come first so the default input is a mic.
inline constexpr MotuPort kUltraLiteCapture[] = {
    {"Mic 1", 2},       {"Mic 2", 3},       {"Analog 3", 4},    {"Analog 4", 5},
    {"Analog 5", 6},    {"Analog 6", 7},    {"Analog 7", 8},    {"Analog 8", 9},
    {"S/PDIF 1", 10},   {"S/PDIF 2", 11},   {"Mix Return L", 0}, {"Mix Return R", 1},
    {"Unused 1", 12},   {"Unused 2", 13},
};

// 828mk2 capture: chunks 0-1 Mix return, 2-9 Analog 1-8, 10-11 Mic 1-2, 12-13 S/PDIF.
// Its analog inputs are numbered from 1, so they keep their physical order.
inline constexpr MotuPort k828mk2Capture[] = {
    {"Analog 1", 2},  {"Analog 2", 3},  {"Analog 3", 4},  {"Analog 4", 5},
    {"Analog 5", 6},  {"Analog 6", 7},  {"Analog 7", 8},  {"Analog 8", 9},
    {"Mic 1", 10},    {"Mic 2", 11},    {"S/PDIF 1", 12}, {"S/PDIF 2", 13},
    {"Mix Return L", 0}, {"Mix Return R", 1},
};

static_assert(IsChunkPermutation(kV2Playback));
static_assert(IsChunkPermutation(kUltraLiteCapture));
static_assert(IsChunkPermutation(k828mk2Capture));

} // namespace ASFW::Encoding::Motu
