// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// MusicSubunitDescriptor.cpp - Implementation of Music Subunit Status Descriptor parser
//

#include "MusicSubunitDescriptor.hpp"

#include <algorithm>

namespace ASFW::Protocols::AVC::Descriptors {

namespace {

inline uint16_t ReadBE16(std::span<const uint8_t> p) noexcept {
    return (static_cast<uint16_t>(p[0]) << 8) | p[1];
}

inline uint32_t ReadBE32(std::span<const uint8_t> p) noexcept {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) |
           static_cast<uint32_t>(p[3]);
}

} // namespace

const MusicSubunitPlug* MusicSubunitStatus::FindPlug(uint8_t plugId, bool isDest) const noexcept {
    for (const auto& plug : plugs) {
        if (plug.plugId == plugId && plug.isDestination == isDest) {
            return &plug;
        }
    }
    return nullptr;
}

const MusicPlugDetail* MusicSubunitStatus::FindMusicPlug(uint16_t musicPlugId) const noexcept {
    for (const auto& mp : musicPlugs) {
        if (mp.musicPlugId == musicPlugId) {
            return &mp;
        }
    }
    return nullptr;
}

std::string MusicSubunitDescriptorParser::ExtractName(const AVCInfoBlock& block) noexcept {
    // 1. Try finding Raw Text Info Block (0x000A)
    std::optional<AVCInfoBlock> nestedRaw;
    const AVCInfoBlock* rawTextBlock = nullptr;
    if (block.GetType() == 0x000A) {
        rawTextBlock = &block;
    } else {
        nestedRaw = block.FindNestedRecursive(0x000A);
        if (nestedRaw.has_value()) {
            rawTextBlock = &*nestedRaw;
        }
    }
    if (rawTextBlock && !rawTextBlock->GetPrimaryData().empty()) {
        const auto& data = rawTextBlock->GetPrimaryData();
        std::string text(reinterpret_cast<const char*>(data.data()), data.size());
        while (!text.empty() && (text.back() == '\0' || text.back() == '\r' || text.back() == '\n')) {
            text.pop_back();
        }
        if (!text.empty()) {
            return text;
        }
    }

    // 2. Try Name Info Block (0x000B)
    std::optional<AVCInfoBlock> nestedName;
    const AVCInfoBlock* nameBlock = nullptr;
    if (block.GetType() == 0x000B) {
        nameBlock = &block;
    } else {
        nestedName = block.FindNestedRecursive(0x000B);
        if (nestedName.has_value()) {
            nameBlock = &*nestedName;
        }
    }
    if (nameBlock && !nameBlock->GetPrimaryData().empty()) {
        const auto& data = nameBlock->GetPrimaryData();

        // 2a. Check for inline 0x000A Info Block inside 0x000B primary data after 4-byte header
        // [0..3]: name info header (e.g. 0x0000ffff)
        // [4..5]: compound_length, [6..7]: type (0x000A), [8..9]: primary_fields_length
        if (data.size() >= 10 && data[6] == 0x00 && data[7] == 0x0A) {
            const uint16_t plen = ReadBE16(std::span<const uint8_t>(data).subspan(8));
            const size_t textLen = std::min(data.size() - 10, static_cast<size_t>(plen));
            std::string text(reinterpret_cast<const char*>(std::span<const uint8_t>(data).subspan(10).data()), textLen);
            while (!text.empty() && (text.back() == '\0' || text.back() == '\r' || text.back() == '\n')) {
                text.pop_back();
            }
            if (!text.empty()) {
                return text;
            }
        }

        // 2b. IEEE 1212 text descriptor header (16 bytes)
        constexpr size_t kTextDescHeaderLen = 16;
        if (data.size() > kTextDescHeaderLen) {
            std::string text(reinterpret_cast<const char*>(std::span<const uint8_t>(data).subspan(kTextDescHeaderLen).data()),
                             data.size() - kTextDescHeaderLen);
            while (!text.empty() && (text.back() == '\0' || text.back() == '\r' || text.back() == '\n')) {
                text.pop_back();
            }
            if (!text.empty()) {
                return text;
            }
        }
    }

    return {};
}

Parsed<MusicSubunitStatus> MusicSubunitDescriptorParser::ParseStatusDescriptor(
    std::span<const uint8_t> data) noexcept {
    auto body = DescriptorBody(data);
    if (!body) return std::unexpected(body.error());
    MusicSubunitStatus status;
    status.declaredLength = static_cast<uint16_t>(data.size() - 2);
    const size_t parseEnd = data.size();
    size_t offset = 2;

    uint8_t destPlugsSeen = 0;

    while (offset < parseEnd) {
        size_t consumed = 0;
        const size_t remaining = parseEnd - offset;
        auto blockResult = AVCInfoBlock::Parse(data.subspan(offset, remaining), consumed, offset);
        if (!blockResult) return std::unexpected(blockResult.error());

        const auto& block = *blockResult;
        const uint16_t type = block.GetType();
        const auto& primaryData = block.GetPrimaryData();

        switch (type) {
            case 0x8100: // GMSSA
                if (primaryData.size() >= 6) {
                    status.capabilities.hasGeneralCapability = true;
                    status.capabilities.transmitCapabilityFlags = primaryData[0];
                    status.capabilities.receiveCapabilityFlags = primaryData[1];
                    status.capabilities.latencyCapability = ReadBE32(std::span<const uint8_t>(primaryData).subspan(2));
                }
                break;

            case 0x8101: // Audio Capability
                if (primaryData.size() >= 5) {
                    status.capabilities.hasAudioCapability = true;
                    status.capabilities.maxAudioInputChannels = ReadBE16(std::span<const uint8_t>(primaryData).subspan(1));
                    status.capabilities.maxAudioOutputChannels = ReadBE16(std::span<const uint8_t>(primaryData).subspan(3));
                }
                // Parse nested 0x8102 Plug Status Area -> 0x8103 Audio Info Area
                for (const auto& plugStatus : block.GetNestedBlocks()) {
                    if (plugStatus.GetType() == 0x8102 && !plugStatus.GetPrimaryData().empty()) {
                        const uint8_t plugId = plugStatus.GetPrimaryData()[0];
                        for (const auto& audioInfo : plugStatus.GetNestedBlocks()) {
                            if (audioInfo.GetType() == 0x8103) {
                                // Labels are separated by CR LF; an unlabelled
                                // stream is an empty entry (TA 2001007 §6.2.3.1).
                                const std::string text = ExtractName(audioInfo);
                                if (!text.empty()) {
                                    std::vector<std::string> names;
                                    size_t start = 0;
                                    while (start < text.size()) {
                                        size_t end = text.find("\r\n", start);
                                        if (end == std::string::npos) {
                                            names.push_back(text.substr(start));
                                            break;
                                        }
                                        names.push_back(text.substr(start, end - start));
                                        start = end + 2;
                                    }
                                    status.perPlugChannelNames[plugId] = std::move(names);
                                }
                            }
                        }
                    }
                }
                break;

            case 0x8102: // MIDI Capability
                if (primaryData.size() >= 6) {
                    status.capabilities.hasMidiCapability = true;
                    status.capabilities.midiVersionMajor = primaryData[0] >> 4;
                    status.capabilities.midiVersionMinor = primaryData[0] & 0x0F;
                    status.capabilities.midiAdaptationLayerVersion = primaryData[1];
                    status.capabilities.maxMidiInputPorts = ReadBE16(std::span<const uint8_t>(primaryData).subspan(2));
                    status.capabilities.maxMidiOutputPorts = ReadBE16(std::span<const uint8_t>(primaryData).subspan(4));
                }
                break;

            case 0x8103: // SMPTE Time Code Capability
                if (!primaryData.empty()) {
                    status.capabilities.hasSmpteTimeCodeCapability = true;
                    status.capabilities.smpteTimeCodeCapabilityFlags = primaryData[0];
                }
                break;

            case 0x8104: // Sample Count Capability
                if (!primaryData.empty()) {
                    status.capabilities.hasSampleCountCapability = true;
                    status.capabilities.sampleCountCapabilityFlags = primaryData[0];
                }
                break;

            case 0x8105: // Audio Sync Capability
                if (!primaryData.empty()) {
                    status.capabilities.hasAudioSyncCapability = true;
                    status.capabilities.audioSyncCapabilityFlags = primaryData[0];
                }
                break;

            case 0x8108: { // Routing Status
                if (primaryData.size() >= 2) {
                    status.hasRoutingStatus = true;
                    status.numDestPlugs = primaryData[0];
                    status.numSrcPlugs = primaryData[1];
                }

                for (const auto& child : block.GetNestedBlocks()) {
                    const uint16_t childType = child.GetType();
                    const auto& childData = child.GetPrimaryData();

                    if (childType == 0x8109 && childData.size() >= 4) { // Subunit Plug Info
                        MusicSubunitPlug plug;
                        plug.plugId = childData[0];
                        plug.usage = childData[3];
                        plug.name = ExtractName(child);

                        // Destination plugs precede source plugs per TA 2001007
                        if (destPlugsSeen < status.numDestPlugs) {
                            plug.isDestination = true;
                            destPlugsSeen++;
                        } else {
                            plug.isDestination = false;
                        }

                        // Child Cluster Info Blocks (0x810A)
                        const auto clusterBlocks = child.FindAllNestedRecursive(0x810A);
                        for (const auto& clusterBlock : clusterBlocks) {
                            const auto& cData = clusterBlock.GetPrimaryData();
                            if (cData.size() < 3) return std::unexpected(ParseError{offset, ParseErrorKind::Truncated});

                            MusicClusterInfo cluster;
                            cluster.streamFormatCode = cData[0];
                            cluster.portType = cData[1];
                            const uint8_t numSignals = cData[2];
                            if (static_cast<size_t>(numSignals) > (cData.size() - 3) / 4)
                                return std::unexpected(ParseError{offset, ParseErrorKind::Truncated});
                            cluster.channelCount = numSignals;
                            cluster.name = ExtractName(clusterBlock);

                            for (uint8_t sigIdx = 0;
                                 sigIdx < numSignals && (3 + (sigIdx + 1) * 4) <= cData.size();
                                 ++sigIdx) {
                                const size_t sigOffset = 3 + sigIdx * 4;
                                cluster.signals.push_back(MusicClusterSignal{
                                    .musicPlugId = ReadBE16(std::span<const uint8_t>(cData).subspan(sigOffset)),
                                    .position = cData[sigOffset + 2],
                                });
                            }
                            plug.clusters.push_back(std::move(cluster));
                        }

                        status.plugs.push_back(std::move(plug));
                    } else if (childType == 0x810B && childData.size() >= 3) { // Music Plug Info
                        MusicPlugDetail mp;
                        mp.portType = childData[0];
                        mp.musicPlugId = ReadBE16(std::span<const uint8_t>(childData).subspan(1));
                        mp.name = ExtractName(child);
                        if (childData.size() >= 14) {
                            const auto endpoint = [&childData](size_t at) {
                                return MusicPlugEndpoint{.functionType = childData[at],
                                                         .plugId = childData[at + 1],
                                                         .functionBlockId = childData[at + 2],
                                                         .streamPosition = childData[at + 3],
                                                         .streamLocation = childData[at + 4]};
                            };
                            mp.source = endpoint(4);
                            mp.destination = endpoint(9);
                        }
                        status.musicPlugs.push_back(std::move(mp));
                    }
                }
                break;
            }

            default:
                break;
        }

        offset += consumed;
    }

    AssignMusicPlugLabels(status);
    return status;
}

void MusicSubunitDescriptorParser::AssignMusicPlugLabels(MusicSubunitStatus& status) {
    constexpr uint8_t kAudioMusicPlug = 0x00;
    for (const auto& [sourcePlug, labels] : status.perPlugChannelNames) {
        std::vector<uint16_t> routed;
        for (const auto& mp : status.musicPlugs) {
            if (mp.portType == kAudioMusicPlug && mp.destination &&
                mp.destination->functionType == MusicPlugEndpoint::kSubunitSourcePlug &&
                mp.destination->plugId == sourcePlug) {
                routed.push_back(mp.musicPlugId);
            }
        }
        std::ranges::sort(routed);
        for (size_t k = 0; k < routed.size() && k < labels.size(); ++k) {
            if (!labels[k].empty()) {
                status.musicPlugLabels[routed[k]] = labels[k];
            }
        }
    }
}

} // namespace ASFW::Protocols::AVC::Descriptors
