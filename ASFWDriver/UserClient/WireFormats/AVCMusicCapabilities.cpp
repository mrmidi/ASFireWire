// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// AVCMusicCapabilities.cpp - see AVCMusicCapabilities.hpp.

#include "AVCMusicCapabilities.hpp"
#include "../../Protocols/AVC/Descriptors/DescriptorTypeCodes.hpp"
#include "../../Shared/SharedDataModels.hpp"

#include <algorithm>
#include <cstring>
#include <string>

namespace ASFW::UserClient::Wire {
namespace {
namespace E = ASFW::AVC::DiscoveryEngine;
namespace A = ASFW::AVC;
using namespace ASFW::Shared;

constexpr uint8_t kUnknownRate = 0xFF;
constexpr uint8_t kAudioPlug = 0x00;
constexpr uint8_t kMidiPlug = 0x01;
constexpr size_t kMaxSupportedFormats = 32;

struct Block { uint8_t formatCode{0}; uint8_t channelCount{0}; };

/// What the blob says about one stream format.
struct Format {
    uint8_t rate{kUnknownRate};
    bool compound{false};
    uint8_t totalChannels{0};   ///< Sum of compound entries, modulo 256 (wire width).
    std::vector<Block> blocks;
};

struct Plug {
    uint8_t id{0};
    bool input{false};
    uint8_t type{0};
    std::string name;
    std::optional<Format> current;
    std::vector<Format> supported;
};

struct Capabilities {
    bool audio{false}, midi{false}, smpte{false};
    std::optional<uint16_t> audioIn, audioOut, midiIn, midiOut;
};

/// TA 2001002 rate codes the app knows; anything else is unknown.
uint8_t RateCode(uint8_t code) {
    switch (code) {
        case 0x00: case 0x01: case 0x02: case 0x03: case 0x04:
        case 0x05: case 0x06: case 0x07: case 0x0A: case 0x0F: return code;
        default: return kUnknownRate;
    }
}

/// AM824 formats the blob describes: compound, or simple (subtype 0x00, 0x01,
/// 0x90). Any other format leaves the plug as the descriptor described it.
bool Describable(const A::Cmd::StreamFormat& format) {
    if (format.kind == A::Cmd::StreamFormat::Kind::kCompoundAm824) return true;
    const auto raw = format.Raw();
    return raw.size() >= 3 && raw[0] == 0x90 && (raw[1] == 0x00 || raw[1] == 0x01 || raw[1] == 0x90);
}

Format FromStreamFormat(const A::Cmd::StreamFormat& format) {
    Format out;
    if (format.kind != A::Cmd::StreamFormat::Kind::kCompoundAm824) return out;
    out.compound = true;
    out.rate = RateCode(static_cast<uint8_t>(format.compound.rate));
    for (const auto& entry : format.compound.Entries()) {
        out.blocks.push_back({static_cast<uint8_t>(entry.format), entry.count});
        out.totalChannels = static_cast<uint8_t>(out.totalChannels + entry.count);
    }
    return out;
}

/// Descriptor clusters as a format: no rate, not compound, blocks only.
Format FromClusters(const std::vector<ASFW::Protocols::AVC::Descriptors::MusicClusterInfo>& clusters) {
    Format out;
    for (const auto& cluster : clusters) out.blocks.push_back({cluster.streamFormatCode, cluster.channelCount});
    return out;
}

uint16_t Channels(const Format& format) {
    if (format.totalChannels > 0) return format.totalChannels;
    uint32_t sum = 0;
    for (const auto& block : format.blocks) sum += block.channelCount;
    return static_cast<uint16_t>(std::min<uint32_t>(sum, 0xFFFFu));
}

/// Channel maxima and port counts from the plugs (applied after the
/// descriptor capabilities, and again once stream formats are known).
void UpdateFromPlugs(Capabilities& caps, const std::vector<Plug>& plugs) {
    if (plugs.empty()) return;
    uint16_t inMax = caps.audioIn.value_or(0), outMax = caps.audioOut.value_or(0);
    uint16_t midiIns = 0, midiOuts = 0;
    for (const auto& plug : plugs) {
        if (plug.type == kAudioPlug) {
            caps.audio = true;
            const uint16_t channels = plug.current ? Channels(*plug.current) : 0;
            (plug.input ? inMax : outMax) = std::max(plug.input ? inMax : outMax, channels);
        } else if (plug.type == kMidiPlug) {
            caps.midi = true;
            ++(plug.input ? midiIns : midiOuts);
        }
    }
    if (inMax > 0) caps.audioIn = inMax;
    if (outMax > 0) caps.audioOut = outMax;
    caps.midiIn = midiIns;
    caps.midiOut = midiOuts;
}

template <class T> void Append(std::vector<uint8_t>& out, const T& wire) {
    const auto* bytes = reinterpret_cast<const uint8_t*>(&wire);
    out.insert(out.end(), bytes, bytes + sizeof(T));
}

void CopyName(char (&field)[32], uint8_t& length, const std::string& name) {
    const size_t copy = std::min(name.size(), sizeof(field) - 1);
    std::memcpy(field, name.data(), copy);
    field[copy] = '\0';
    length = static_cast<uint8_t>(copy);
}
} // namespace

std::optional<std::vector<uint8_t>> BuildMusicCapabilities(const E::DiscoverySnapshot& snapshot,
                                                           A::SubunitId subunit, size_t maxBytes) {
    if (!snapshot.unit.FindSubunit(subunit.type, subunit.id)) return std::nullopt;

    Capabilities caps;
    std::vector<Plug> plugs;
    const auto contents = std::find_if(snapshot.contents.begin(), snapshot.contents.end(),
                                       [&](const auto& c) { return c.id == subunit; });
    // The identifier descriptor's static capabilities (TA 2001007 §5.2), first: the plugs below replace the
    // channel and port counts with what the subunit really exposes.
    if (contents != snapshot.contents.end() && contents->musicIdentifier) {
        const auto& id = *contents->musicIdentifier;
        if (id.audio && !id.audio->empty()) {
            caps.audio = true;
            uint16_t in = 0, out = 0;
            for (const auto& format : *id.audio) { in = std::max(in, format.maxInputChannels); out = std::max(out, format.maxOutputChannels); }
            caps.audioIn = in; caps.audioOut = out;
        }
        if (id.midi) { caps.midi = true; caps.midiIn = id.midi->maxInputPorts; caps.midiOut = id.midi->maxOutputPorts; }
        if (id.smpteTimeCode && *id.smpteTimeCode != 0) caps.smpte = true;
    }
    if (contents != snapshot.contents.end() && contents->music) {
        const auto& status = *contents->music;
        // SMPTE time code is present when the subunit describes an SMPTE music plug (810B, type 02) or a
        // source plug reports SMPTE activity (8105, §6.2.3.3).
        for (const auto& mp : status.musicPlugs) {
            if (mp.plugType == Protocols::AVC::Descriptors::kMusicPlugTypeSmpte) caps.smpte = true;
        }
        for (const auto& [plugId, activity] : status.perPlugActivity) {
            if (activity.smpteTimeCode) caps.smpte = true;
        }
        for (const auto& p : status.plugs) {
            Plug plug{.id = p.plugId, .input = p.isDestination,
                      // Analog and digital audio usage are audio streams (Apple MusicSubunitController.h:96-105).
                      .type = (p.usage == Protocols::AVC::Descriptors::kMusicPlugUsageAnalogAudio ||
                               p.usage == Protocols::AVC::Descriptors::kMusicPlugUsageDigitalAudio)
                                  ? kAudioPlug : p.usage, .name = p.name};
            if (!p.clusters.empty()) plug.current = FromClusters(p.clusters);
            plugs.push_back(std::move(plug));
        }
        UpdateFromPlugs(caps, plugs);
    }

    // Discovered stream formats replace the descriptor's view of each plug.
    const auto address = subunit.ToAddress();
    for (const auto& fact : snapshot.plugs) {
        if (fact.address != address) continue;
        const bool input = fact.direction == A::Cmd::PlugDirection::kInput;
        const auto plug = std::find_if(plugs.begin(), plugs.end(),
                                       [&](const Plug& p) { return p.id == fact.id.value && p.input == input; });
        if (plug == plugs.end()) continue;
        if (fact.current && Describable(*fact.current)) plug->current = FromStreamFormat(*fact.current);
        for (const auto& formation : fact.formations)
            if (Describable(formation)) plug->supported.push_back(FromStreamFormat(formation));
    }
    UpdateFromPlugs(caps, plugs);

    AVCMusicCapabilitiesWire header{};
    header.hasAudio = caps.audio ? 1 : 0;
    header.hasMIDI = caps.midi ? 1 : 0;
    header.hasSMPTE = caps.smpte ? 1 : 0;
    header.audioInputPorts = static_cast<uint8_t>(caps.audioIn.value_or(0));
    header.audioOutputPorts = static_cast<uint8_t>(caps.audioOut.value_or(0));
    header.midiInputPorts = static_cast<uint8_t>(caps.midiIn.value_or(0));
    header.midiOutputPorts = static_cast<uint8_t>(caps.midiOut.value_or(0));
    header.currentRate = kUnknownRate;
    for (const auto& plug : plugs) {
        if (header.currentRate == kUnknownRate && plug.current && plug.current->rate != kUnknownRate)
            header.currentRate = plug.current->rate;
        for (const auto& format : plug.supported)
            if (format.rate < 32) header.supportedRatesMask |= 1u << format.rate;
    }

    std::vector<uint8_t> body;
    size_t total = sizeof(header);
    for (const auto& plug : plugs) {
        const auto blocks = plug.current && plug.current->compound
            ? std::min<size_t>(plug.current->blocks.size(), 255) : size_t{0};
        const auto formats = std::min(plug.supported.size(), kMaxSupportedFormats);
        const size_t size = sizeof(PlugInfoWire) + blocks * sizeof(SignalBlockWire) + formats * sizeof(SupportedFormatWire);
        if (total + size > maxBytes) break;
        total += size;
        ++header.numPlugs;

        PlugInfoWire wire{};
        wire.plugID = plug.id;
        wire.isInput = plug.input ? 1 : 0;
        wire.type = plug.type;
        wire.numSignalBlocks = static_cast<uint8_t>(blocks);
        wire.numSupportedFormats = static_cast<uint8_t>(formats);
        CopyName(wire.name, wire.nameLength, plug.name);
        Append(body, wire);
        for (size_t b = 0; b < blocks; ++b) {
            SignalBlockWire block{};
            block.formatCode = plug.current->blocks[b].formatCode;
            block.channelCount = plug.current->blocks[b].channelCount;
            Append(body, block);
        }
        for (size_t f = 0; f < formats; ++f) {
            const auto& format = plug.supported[f];
            SupportedFormatWire supported{};
            supported.sampleRateCode = format.rate;
            supported.formatCode = format.blocks.empty() ? 0x06 : format.blocks[0].formatCode; // MBLA default.
            supported.channelCount = format.totalChannels;
            Append(body, supported);
        }
    }

    std::vector<uint8_t> out;
    out.reserve(total);
    Append(out, header);
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

} // namespace ASFW::UserClient::Wire
