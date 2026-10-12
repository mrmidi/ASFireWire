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

#include "MotuModel.hpp"

#include <array>
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
    /// PCM chunks per data block, as resolved by MotuProtocol::PrepareDuplex.
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

// Physical group order is cross-checked against FFADO motu_avdevice.cpp:91–280.
// Unknown groups stay visible as unknown; only explicitly identified padding is hidden.
// Layouts are immutable static storage: the RX/TX queues retain spans, never a
// temporary vector or a view into another service's mutable state.
struct PortLayout final {
    std::array<MotuPort, 34> ports{};
    uint32_t wireChunks{0};
    uint32_t hostChannels{0};
    [[nodiscard]] constexpr MotuPortMap Map() const noexcept {
        return {ports.data(), wireChunks};
    }
};

namespace Detail {
inline constexpr const char* kAnalogNames[] = {
    "Analog 1", "Analog 2", "Analog 3", "Analog 4", "Analog 5", "Analog 6", "Analog 7", "Analog 8"};
inline constexpr const char* kOpticalNames[] = {
    "Optical 1", "Optical 2", "Optical 3", "Optical 4", "Optical 5", "Optical 6", "Optical 7", "Optical 8",
    "Optical 9", "Optical 10", "Optical 11", "Optical 12", "Optical 13", "Optical 14", "Optical 15", "Optical 16"};

[[nodiscard]] constexpr PortLayout BuildPortLayout(uint32_t version, uint32_t mode,
                                                   uint32_t extra, bool capture) noexcept {
    PortLayout out{};
    const auto* model = FindModel(version);
    if (!model || mode >= 3 || (model->unresolvedModes & (1U << mode))) return out;
    const uint32_t fixed = capture ? model->captureChunks[mode] : model->playbackChunks[mode];
    if (!fixed || fixed + extra > out.ports.size()) return out;
    out.wireChunks = fixed + extra;
    uint32_t count = 0;
    const auto add = [&](const char* name, uint32_t chunk) {
        if (chunk < out.wireChunks) out.ports[count++] = {name, static_cast<uint8_t>(chunk)};
    };
    const auto pair = [&](const char* left, const char* right, uint32_t chunk) {
        add(left, chunk); add(right, chunk + 1);
    };
    const auto analog = [&](uint32_t first) {
        for (uint32_t i = 0; i < 8; ++i) add(kAnalogNames[i], first + i);
    };
    const auto optical = [&](uint32_t first) {
        for (uint32_t i = 0; i < extra; ++i) add(kOpticalNames[i], first + i);
    };
    const auto mix = [&](uint32_t first) { pair("Return L", "Return R", first); };
    const auto phones = [&](uint32_t first) { pair("Phones L", "Phones R", first); };
    const auto main = [&](uint32_t first) { pair("Main L", "Main R", first); };
    const auto spdif = [&](uint32_t first) { pair("S/PDIF 1", "S/PDIF 2", first); };
    const auto aes = [&](uint32_t first) { pair("AES/EBU 1", "AES/EBU 2", first); };
    const auto reverb = [&](uint32_t first) { pair("Reverb L", "Reverb R", first); };
    uint64_t padding = 0;
    if (version == 1) {
        analog(0); spdif(8); optical(fixed);
    } else if (version == 3 || version == 0xd) {
        const auto base = capture ? (version == 3 ? MotuPortMap{k828mk2Capture} : MotuPortMap{kUltraLiteCapture})
                                  : MotuPortMap{kV2Playback};
        for (const auto& port : base) {
            if (capture && version == 0xd && port.chunk >= 12) continue;
            add(port.name, port.chunk);
        }
        optical(fixed);
        if (capture && version == 0xd) padding = (uint64_t{3} << 12);
    } else if (version == 5) {
        if (mode == 2) analog(0);
        else if (capture) {
            analog(2); aes(out.wireChunks - 2); mix(0);
            pair("Unknown 1", "Unknown 2", 10); optical(12);
        } else {
            main(10); analog(2); aes(out.wireChunks - 2); phones(0); optical(12);
        }
    } else if (version == 9) {
        if (mode == 2) analog(0);
        else {
            analog(2); aes(10); pair("Digital 1", "Digital 2", 12);
            // Traveler switches this pair between coax S/PDIF and optical Toslink.
            if (capture) mix(0); else phones(0);
            optical(fixed);
        }
    } else if (version == 0xf) {
        if (capture) { analog(2); mix(0); optical(fixed); }
        else { main(2); phones(0); optical(fixed); padding = uint64_t{3} << 4; }
    } else if (version == 0x19) {
        if (capture) {
            analog(2); if (mode < 2) spdif(10); mix(0);
            if (mode < 2) reverb(12);
            if (mode == 0) padding = uint64_t{15} << 14;
        } else {
            main(0); analog(2); if (mode < 2) spdif(12); phones(10);
            if (mode == 2) padding = uint64_t{3} << 12;
        }
    } else if (version == 0x15) {
        if (capture) {
            pair("Mic 1", "Mic 2", 0); analog(2);
            if (mode < 2) spdif(12);
            mix(10); if (mode < 2) reverb(14);
            // FFADO puts the 2x unknown pair after the optical banks.
            optical(mode == 1 ? 16 : fixed);
        } else if (mode < 2) {
            main(10); analog(2); phones(0); spdif(12); optical(fixed);
        } else {
            // No Main pair is identified at 4x; never label unknown slots as Main.
            analog(2);
        }
    } else if (version == 0x1b) {
        // FFADO's optical details are unfinished; fixed known groups are named,
        // and the optical/unknown slots remain explicit rather than guessed jacks.
        analog(2); if (mode < 2) { aes(10); spdif(12); }
        if (capture) { mix(0); if (mode == 0) reverb(14); }
        else phones(0);
        optical(fixed);
    } else if (version == 0x17) {
        // FFADO calls this table provisional. Preserve the fixed wire positions
        // with generic names; do not install an unverified physical permutation.
        for (uint32_t chunk = 0; chunk < fixed; ++chunk) add("Unknown", chunk);
        optical(fixed);
    } else if (version == 2) {
        for (uint32_t chunk = 0; chunk < fixed; ++chunk) add("Unknown", chunk);
        optical(fixed); // No original-896 physical group table in FFADO.
    }
    // Preserve unclassified slots; unlike known padding these remain host channels.
    uint64_t used = padding;
    for (uint32_t i = 0; i < count; ++i) used |= uint64_t{1} << out.ports[i].chunk;
    for (uint32_t chunk = 0; chunk < out.wireChunks; ++chunk)
        if (!(used & (uint64_t{1} << chunk))) add("Unknown", chunk);
    out.hostChannels = count;
    // Padding is last in the permutation, so the existing writer explicitly
    // zeros it when the host buffer contains only the visible channels.
    for (uint32_t chunk = 0; chunk < out.wireChunks; ++chunk)
        if (padding & (uint64_t{1} << chunk)) add("Unused", chunk);
    return out;
}

// Only retain formations supported by the model, with optical widths 0/4/8/12/16.
// The corpus supplies lifetime-safe maps without allocations on RX/TX hot paths.
inline constexpr auto kPortLayouts = [] {
    std::array<PortLayout, 11 * 3 * 2 * 5> result{};
    uint32_t i = 0;
    for (const auto& model : kModels) {
        if (!FireWireOnly(model.unitVersion)) continue;
        for (uint32_t mode = 0; mode < 3; ++mode)
            for (bool capture : {false, true})
                for (uint32_t extra : {0U, 4U, 8U, 12U, 16U})
                    result[i++] = BuildPortLayout(model.unitVersion, mode, extra, capture);
    }
    return result;
}();
} // namespace Detail

[[nodiscard]] constexpr const PortLayout* PortLayoutFor(uint32_t version, uint32_t rate,
                                                        uint32_t wireChunks, bool capture) noexcept {
    const auto* model = FindModel(version);
    const auto index = RateToIndex(rate);
    if (!model || !FireWireOnly(version) || index < 0) return nullptr;
    const auto mode = IndexToMode(static_cast<uint32_t>(index));
    const auto fixed = capture ? model->captureChunks[mode] : model->playbackChunks[mode];
    if (!wireChunks) wireChunks = fixed;
    if (wireChunks < fixed || (wireChunks - fixed) % 4 || wireChunks - fixed > 16) return nullptr;
    uint32_t modelIndex = 0;
    for (const auto& entry : kModels) {
        if (!FireWireOnly(entry.unitVersion)) continue;
        if (entry.unitVersion == version) break;
        ++modelIndex;
    }
    const auto& layout = Detail::kPortLayouts[((modelIndex * 3 + mode) * 2 + (capture ? 1 : 0)) * 5 + (wireChunks - fixed) / 4];
    return layout.wireChunks == wireChunks && layout.hostChannels ? &layout : nullptr;
}

[[nodiscard]] constexpr uint32_t HostChannelsFor(uint32_t version, uint32_t rate,
                                                uint32_t chunks, bool capture) noexcept {
    const auto* layout = PortLayoutFor(version, rate, chunks, capture);
    return layout ? layout->hostChannels : chunks;
}

} // namespace ASFW::Encoding::Motu
