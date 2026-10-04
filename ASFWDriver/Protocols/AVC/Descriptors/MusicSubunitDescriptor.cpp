// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// MusicSubunitDescriptor.cpp - Implementation of Music Subunit Status Descriptor parser
//

#include "MusicSubunitDescriptor.hpp"
#include "DescriptorTypeCodes.hpp"

#include <algorithm>
#include <numeric>

namespace ASFW::Protocols::AVC::Descriptors {

namespace {
/// Trailing NUL/CR/LF are padding, not text.
std::string TrimmedText(std::span<const uint8_t> bytes) {
    std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    while (!text.empty() && (text.back() == '\0' || text.back() == '\r' || text.back() == '\n')) text.pop_back();
    return text;
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
    if (block.GetType() == kInfoBlockRawText) {
        rawTextBlock = &block;
    } else {
        nestedRaw = block.FindNestedRecursive(kInfoBlockRawText);
        if (nestedRaw.has_value()) {
            rawTextBlock = &*nestedRaw;
        }
    }
    if (rawTextBlock && !rawTextBlock->GetPrimaryData().empty()) {
        if (auto text = TrimmedText(rawTextBlock->GetPrimaryData()); !text.empty()) return text;
    }

    // 2. Try Name Info Block (0x000B)
    std::optional<AVCInfoBlock> nestedName;
    const AVCInfoBlock* nameBlock = nullptr;
    if (block.GetType() == kInfoBlockName) {
        nameBlock = &block;
    } else {
        nestedName = block.FindNestedRecursive(kInfoBlockName);
        if (nestedName.has_value()) {
            nameBlock = &*nestedName;
        }
    }
    if (nameBlock && !nameBlock->GetPrimaryData().empty()) {
        const auto& data = nameBlock->GetPrimaryData();

        // 2a. An inline raw-text info block after the 4-byte name info header:
        // [0..3] header, [4..5] compound_length, [6..7] type 0x000A,
        // [8..9] primary_fields_length, then the text (clipped to what exists).
        ParseReader inline_(data);
        uint16_t compoundLength{}, inlineType{}, textLength{};
        if (inline_.Take(4) && inline_.Fields(compoundLength, inlineType, textLength) && inlineType == kInfoBlockRawText) {
            if (auto bytes = inline_.Take(std::min<size_t>(inline_.Remaining(), textLength))) {
                if (auto text = TrimmedText(*bytes); !text.empty()) return text;
            }
        }

        // 2b. IEEE 1212 text descriptor header (16 bytes)
        constexpr size_t kTextDescHeaderLen = 16;
        if (data.size() > kTextDescHeaderLen) {
            if (auto text = TrimmedText(std::span<const uint8_t>(data).subspan(kTextDescHeaderLen)); !text.empty())
                return text;
        }
    }

    return {};
}

std::vector<std::string> MusicSubunitDescriptorParser::SplitLabels(const std::string& text) {
    std::vector<std::string> labels;
    size_t start = 0;
    while (start < text.size()) {
        const size_t end = text.find("\r\n", start);
        if (end == std::string::npos) {
            labels.push_back(text.substr(start));
            break;
        }
        labels.push_back(text.substr(start, end - start));
        start = end + 2;
    }
    if (labels.empty()) labels.emplace_back();  // a stream with no text keeps its place
    return labels;
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
        status.topLevelBlocks.push_back(InfoBlockSeen{type, static_cast<uint16_t>(consumed),
                                                      static_cast<uint16_t>(primaryData.size())});

        switch (type) {
            case kMusicInfoBlockGeneralStatusArea: {
                auto& caps = status.capabilities;
                uint8_t tx{}, rx{}; uint32_t latency{};
                if (ParseReader(primaryData).Fields(tx, rx, latency)) {
                    caps.hasGeneralCapability = true;
                    caps.transmitCapabilityFlags = tx; caps.receiveCapabilityFlags = rx; caps.latencyCapability = latency;
                }
                break;
            }

            case kMusicInfoBlockOutputPlugStatusArea: {
                // Music Output Plug Status Area (TA 2001007 §6.2.2): number_of_source_plugs, then one nested 8102
                // per source plug it describes. Phase 88 sends 4 with source plugs 0, 1, 2 and 5 (SUBUNIT INFO
                // says 6): the count is of the plugs described, and their numbers are not 0..n-1.
                if (!primaryData.empty()) status.declaredSourcePlugs = primaryData[0];
                // Parse nested 0x8102 Plug Status Area -> 0x8103 Audio Info Area
                for (const auto& plugStatus : block.GetNestedBlocks()) {
                    if (plugStatus.GetType() == kMusicInfoBlockSourcePlugStatus && !plugStatus.GetPrimaryData().empty()) {
                        const uint8_t plugId = plugStatus.GetPrimaryData()[0];
                        for (const auto& info : plugStatus.GetNestedBlocks()) {
                            if (info.GetType() == kMusicInfoBlockAudioInfo) {
                                // An unlabelled stream is an empty entry (TA 2001007 §6.2.3.1).
                                if (const std::string text = ExtractName(info); !text.empty()) {
                                    status.perPlugChannelNames[plugId] = SplitLabels(text);
                                }
                            } else if (info.GetType() == kMusicInfoBlockSmpteTimeCodeInfo ||
                                       info.GetType() == kMusicInfoBlockSampleCountInfo ||
                                       info.GetType() == kMusicInfoBlockAudioSyncInfo) {
                                // SMPTE time code, sample count and audio SYNC activity (§6.2.3.3-§6.2.3.5): one byte.
                                if (info.GetPrimaryData().empty()) continue;
                                auto& activity = status.perPlugActivity[plugId];
                                const uint8_t bits = info.GetPrimaryData()[0];
                                if (info.GetType() == kMusicInfoBlockSmpteTimeCodeInfo) activity.smpteTimeCode = bits;
                                else if (info.GetType() == kMusicInfoBlockSampleCountInfo) activity.sampleCount = bits;
                                else activity.audioSync = bits;
                            } else if (info.GetType() == kMusicInfoBlockMidiInfo) {
                                // number_of_MIDI_streams, then one name_info_block per stream (§6.2.3.2).
                                MusicMidiStreams midi;
                                if (!info.GetPrimaryData().empty()) midi.declaredStreams = info.GetPrimaryData()[0];
                                for (const auto& name : info.GetNestedBlocks()) {
                                    if (name.GetType() != kInfoBlockName) continue;
                                    for (auto& label : SplitLabels(ExtractName(name))) midi.labels.push_back(std::move(label));
                                }
                                status.perPlugMidiStreams[plugId] = std::move(midi);
                            }
                        }
                    }
                }
                break;
            }

            case kMusicInfoBlockRoutingStatus: {
                if (primaryData.size() >= 2) {
                    status.hasRoutingStatus = true;
                    status.numDestPlugs = primaryData[0];
                    status.numSrcPlugs = primaryData[1];
                }

                for (const auto& child : block.GetNestedBlocks()) {
                    const uint16_t childType = child.GetType();
                    const auto& childData = child.GetPrimaryData();

                    if (childType == kMusicInfoBlockSubunitPlugInfo && childData.size() >= 4) { // Subunit Plug Info
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
                        const auto clusterBlocks = child.FindAllNestedRecursive(kMusicInfoBlockClusterInfo);
                        for (const auto& clusterBlock : clusterBlocks) {
                            // Cluster info (TA 2001007 Table 6.10): format, port type,
                            // signal count, then 4 bytes per signal.
                            ParseReader cluster(clusterBlock.GetPrimaryData(), offset);
                            MusicClusterInfo info;
                            uint8_t numSignals{};
                            if (!cluster.Fields(info.streamFormatCode, info.portType, numSignals))
                                return std::unexpected(ParseError{offset, ParseErrorKind::Truncated});
                            info.channelCount = numSignals;
                            info.name = ExtractName(clusterBlock);
                            for (uint8_t signal = 0; signal < numSignals; ++signal) {
                                MusicClusterSignal entry;
                                uint8_t location{};
                                if (!cluster.Fields(entry.musicPlugId, entry.position, location))
                                    return std::unexpected(ParseError{offset, ParseErrorKind::Truncated});
                                info.signals.push_back(entry);
                            }
                            plug.clusters.push_back(std::move(info));
                        }

                        status.plugs.push_back(std::move(plug));
                    } else if (childType == kMusicInfoBlockMusicPlugInfo) { // Music Plug Info (TA 2001007 Table 6.11)
                        ParseReader fields(childData);
                        MusicPlugDetail mp;
                        if (!fields.Fields(mp.plugType, mp.musicPlugId)) continue;
                        mp.name = ExtractName(child);
                        const auto endpoint = [&fields]() -> std::optional<MusicPlugEndpoint> {
                            MusicPlugEndpoint e;
                            if (!fields.Fields(e.functionType, e.plugId, e.functionBlockId, e.streamPosition, e.streamLocation))
                                return std::nullopt;
                            return e;
                        };
                        // routing_support, then source and destination (5 bytes each); FFADO
                        // avc_descriptor_music.cpp:455-466, Apple MusicSubunitController.h:156-165.
                        if (fields.Remaining() >= 11 && fields.Fields(mp.routingSupport)) {
                            mp.source = endpoint();
                            mp.destination = endpoint();
                        }
                        status.musicPlugs.push_back(std::move(mp));
                    }
                }
                break;
            }

            default:
                break;
        }

        offset = std::add_sat(offset, consumed);
    }

    AssignMusicPlugLabels(status);
    return status;
}

void MusicSubunitDescriptorParser::AssignMusicPlugLabels(MusicSubunitStatus& status) {
    for (const auto& [sourcePlug, labels] : status.perPlugChannelNames) {
        std::vector<uint16_t> routed;
        for (const auto& mp : status.musicPlugs) {
            if (mp.plugType == kMusicPlugTypeAudio && mp.destination &&
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
