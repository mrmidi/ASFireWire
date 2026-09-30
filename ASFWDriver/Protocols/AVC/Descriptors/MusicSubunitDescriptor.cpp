// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// MusicSubunitDescriptor.cpp - Implementation of Music Subunit Status Descriptor parser
//

#include "MusicSubunitDescriptor.hpp"

#include <algorithm>

namespace ASFW::Protocols::AVC::Descriptors {

namespace {

inline uint16_t ReadBE16(const uint8_t* p) noexcept {
    return (static_cast<uint16_t>(p[0]) << 8) | p[1];
}

inline uint32_t ReadBE32(const uint8_t* p) noexcept {
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
    const auto rawTextBlock = block.FindNestedRecursive(0x000A);
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
    const auto nameBlock = block.FindNestedRecursive(0x000B);
    if (nameBlock && nameBlock->GetPrimaryData().size() >= 16) {
        const auto& data = nameBlock->GetPrimaryData();
        // Skip text descriptor header (16 bytes per IEEE 1212 / TA 1999045 §5.2)
        const size_t textOffset = 16;
        if (textOffset < data.size()) {
            std::string text(reinterpret_cast<const char*>(data.data() + textOffset),
                             data.size() - textOffset);
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

std::optional<MusicSubunitStatus> MusicSubunitDescriptorParser::ParseStatusDescriptor(
    std::span<const uint8_t> data) noexcept {
    if (data.size() < 2) {
        return std::nullopt;
    }

    MusicSubunitStatus status;
    status.declaredLength = ReadBE16(data.data());

    const size_t advertisedEnd = 2 + static_cast<size_t>(status.declaredLength);
    const size_t parseEnd = std::min(data.size(), advertisedEnd);
    size_t offset = 2;

    uint8_t destPlugsSeen = 0;

    while (offset + 6 <= parseEnd) {
        size_t consumed = 0;
        const size_t remaining = parseEnd - offset;
        auto blockResult = AVCInfoBlock::Parse(data.data() + offset, remaining, consumed);
        if (!blockResult || consumed == 0) {
            // Skip invalid quadlet and continue scanning
            offset += 4;
            continue;
        }

        const auto& block = *blockResult;
        const uint16_t type = block.GetType();
        const auto& primaryData = block.GetPrimaryData();

        switch (type) {
            case 0x8100: // GMSSA
                if (primaryData.size() >= 6) {
                    status.capabilities.hasGeneralCapability = true;
                    status.capabilities.transmitCapabilityFlags = primaryData[0];
                    status.capabilities.receiveCapabilityFlags = primaryData[1];
                    status.capabilities.latencyCapability = ReadBE32(primaryData.data() + 2);
                }
                break;

            case 0x8101: // Audio Capability
                if (primaryData.size() >= 5) {
                    status.capabilities.hasAudioCapability = true;
                    status.capabilities.maxAudioInputChannels = ReadBE16(primaryData.data() + 1);
                    status.capabilities.maxAudioOutputChannels = ReadBE16(primaryData.data() + 3);
                }
                break;

            case 0x8102: // MIDI Capability
                if (primaryData.size() >= 6) {
                    status.capabilities.hasMidiCapability = true;
                    status.capabilities.midiVersionMajor = primaryData[0] >> 4;
                    status.capabilities.midiVersionMinor = primaryData[0] & 0x0F;
                    status.capabilities.midiAdaptationLayerVersion = primaryData[1];
                    status.capabilities.maxMidiInputPorts = ReadBE16(primaryData.data() + 2);
                    status.capabilities.maxMidiOutputPorts = ReadBE16(primaryData.data() + 4);
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
                            if (cData.size() < 3) continue;

                            MusicClusterInfo cluster;
                            cluster.streamFormatCode = cData[0];
                            const uint8_t numSignals = cData[2];
                            cluster.channelCount = numSignals;
                            cluster.name = ExtractName(clusterBlock);

                            for (uint8_t sigIdx = 0;
                                 sigIdx < numSignals && (3 + (sigIdx + 1) * 4) <= cData.size();
                                 ++sigIdx) {
                                const size_t sigOffset = 3 + sigIdx * 4;
                                cluster.signals.push_back(MusicClusterSignal{
                                    .musicPlugId = ReadBE16(cData.data() + sigOffset),
                                    .position = cData[sigOffset + 2],
                                });
                            }
                            plug.clusters.push_back(std::move(cluster));
                        }

                        status.plugs.push_back(std::move(plug));
                    } else if (childType == 0x810B && childData.size() >= 4) { // Music Plug Info
                        MusicPlugDetail mp;
                        mp.portType = childData[0];
                        mp.musicPlugId = ReadBE16(childData.data() + 2);
                        mp.name = ExtractName(child);
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

    return status;
}

} // namespace ASFW::Protocols::AVC::Descriptors
