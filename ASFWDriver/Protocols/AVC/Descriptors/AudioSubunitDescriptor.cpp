// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 ASFireWire Project
//
// AudioSubunitDescriptor.cpp - AV/C Audio Subunit Descriptor Parser
//

#include "AudioSubunitDescriptor.hpp"

#include <algorithm>
#include <cstring>

namespace ASFW::Protocols::AVC::Descriptors {

namespace {

[[nodiscard]] inline uint16_t ReadBE16(const uint8_t* p) noexcept {
    return (static_cast<uint16_t>(p[0]) << 8) | p[1];
}

} // namespace

std::optional<AudioSubunitIdentifier> AudioSubunitDescriptorParser::ParseIdentifierDescriptor(
    std::span<const uint8_t> data) noexcept {
    // Descriptor header:
    // [0..1]: descriptor_length (2)
    // [2]: generation_ID (1)
    // [3]: size_of_list_ID (1)
    // [4]: size_of_object_ID (1)
    // [5]: size_of_object_position (1)
    // [6..7]: number_of_root_object_lists (2)
    if (data.size() < 8) {
        return std::nullopt;
    }

    AudioSubunitIdentifier id{};
    id.generationId = data[2];
    id.sizeOfListId = data[3];
    id.sizeOfObjectId = data[4];
    id.sizeOfObjectPosition = data[5];

    const uint16_t numRootLists = ReadBE16(&data[6]);
    size_t offset = 8;

    const size_t listIdSize = id.sizeOfListId > 0 ? id.sizeOfListId : 2;
    for (uint16_t i = 0; i < numRootLists; ++i) {
        if (offset + listIdSize > data.size()) {
            return std::nullopt;
        }
        if (listIdSize == 2) {
            id.rootListIds.push_back(ReadBE16(&data[offset]));
        }
        offset += listIdSize;
    }

    if (offset + 2 > data.size()) {
        return id;
    }

    const uint16_t subunitDepLen = ReadBE16(&data[offset]);
    offset += 2;
    const size_t subunitDepEnd = std::min(data.size(), offset + subunitDepLen);

    if (offset + 2 > subunitDepEnd) {
        return id;
    }

    const uint16_t configDepLen = ReadBE16(&data[offset]);
    offset += 2;
    const size_t configDepEnd = std::min(subunitDepEnd, offset + configDepLen);

    if (offset + 4 > configDepEnd) {
        return id;
    }

    // Skip configuration_ID (2)
    offset += 2;

    const uint16_t configInfoLen = ReadBE16(&data[offset]);
    offset += 2;
    const size_t configInfoEnd = std::min(configDepEnd, offset + configInfoLen);

    if (offset + 4 > configInfoEnd) {
        return id;
    }

    // Skip configuration_name (2)
    offset += 2;

    // Cluster information: [cluster_info_len: 2][cluster_info...]
    const uint16_t clusterLen = ReadBE16(&data[offset]);
    offset += 2 + clusterLen;

    if (offset >= configInfoEnd) {
        return id;
    }

    // Subunit source plugs count and links
    const uint8_t numSourcePlugs = data[offset++];
    for (uint8_t i = 0; i < numSourcePlugs; ++i) {
        if (offset + 2 > configInfoEnd) {
            return std::nullopt;
        }
        id.sourcePlugLinks.push_back(AudioSourceId{
            .type = data[offset],
            .id = data[offset + 1],
        });
        offset += 2;
    }

    if (offset >= configInfoEnd) {
        return id;
    }

    // Function blocks count
    const uint8_t numFunctionBlocks = data[offset++];

    for (uint8_t fbIdx = 0; fbIdx < numFunctionBlocks && offset + 6 <= configInfoEnd; ++fbIdx) {
        const uint16_t fbLen = ReadBE16(&data[offset]);
        offset += 2;
        const size_t fbEnd = std::min(configInfoEnd, offset + fbLen);

        if (offset + 4 > fbEnd) {
            offset = fbEnd;
            continue;
        }

        AudioFunctionBlockInfo fbInfo{};
        fbInfo.type = static_cast<AudioFunctionBlockType>(data[offset]);
        fbInfo.id = data[offset + 1];
        fbInfo.nameIndex = ReadBE16(&data[offset + 2]);
        offset += 4;

        if (offset < fbEnd) {
            const uint8_t numInputs = data[offset++];
            for (uint8_t inIdx = 0; inIdx < numInputs && offset + 2 <= fbEnd; ++inIdx) {
                fbInfo.inputSources.push_back(AudioSourceId{
                    .type = data[offset],
                    .id = data[offset + 1],
                });
                offset += 2;
            }
        }

        // Cluster info
        if (offset + 2 <= fbEnd) {
            const uint16_t fbClusterLen = ReadBE16(&data[offset]);
            offset += 2;
            if (fbClusterLen > 0 && offset < fbEnd) {
                fbInfo.clusterChannels = data[offset];
            }
            offset += fbClusterLen;
        }

        // Type-dependent info (Controls for Feature block, or Process type for Processing block)
        if (offset + 2 <= fbEnd) {
            const uint16_t typeDepLen = ReadBE16(&data[offset]);
            offset += 2;
            const size_t typeDepEnd = std::min(fbEnd, offset + typeDepLen);

            if (fbInfo.type == AudioFunctionBlockType::kFeature && offset < typeDepEnd) {
                // Table 8.3: Feature function block dependent information
                // Both 8-bit length (Duet) and 16-bit length (Phase 88) are found in hardware:
                uint8_t sizeOfControls = 2;
                if (data[offset] != 0) {
                    // 8-bit length format (e.g. Duet: 0x08 0x02 0x00 ...)
                    // offset+0: controls_specific_information_length (1 byte)
                    // offset+1: size_of_controls (1 byte)
                    // offset+2: general_tag (1 byte)
                    if (offset + 3 <= typeDepEnd) {
                        sizeOfControls = data[offset + 1];
                        fbInfo.generalTag = data[offset + 2];
                        offset += 3;
                    }
                } else {
                    // 16-bit length format (e.g. Phase 88: 0x0015 0x0002 ...)
                    // offset+0..1: controls_specific_information_length (2 bytes BE)
                    // offset+2..3: size_of_controls (2 bytes BE)
                    if (offset + 4 <= typeDepEnd) {
                        sizeOfControls = static_cast<uint8_t>(ReadBE16(&data[offset + 2]));
                        offset += 4;
                    }
                }

                if (sizeOfControls > 0 && offset + sizeOfControls <= typeDepEnd) {
                    uint16_t masterCtrl = 0;
                    if (sizeOfControls == 1) {
                        masterCtrl = data[offset];
                    } else {
                        masterCtrl = ReadBE16(&data[offset]);
                    }
                    fbInfo.masterControls = masterCtrl;
                    offset += sizeOfControls;

                    // Channel controls
                    while (offset + sizeOfControls <= typeDepEnd) {
                        uint16_t chCtrl = 0;
                        if (sizeOfControls == 1) {
                            chCtrl = data[offset];
                        } else {
                            chCtrl = ReadBE16(&data[offset]);
                        }
                        fbInfo.channelControls.push_back(chCtrl);
                        offset += sizeOfControls;
                    }
                }
            } else if (fbInfo.type == AudioFunctionBlockType::kProcessing && offset < typeDepEnd) {
                fbInfo.processType = data[offset];
            }
            offset = typeDepEnd;
        }

        id.functionBlocks.push_back(std::move(fbInfo));
        offset = fbEnd;
    }

    return id;
}

TextDatabase AudioSubunitDescriptorParser::ParseTextDatabaseList(
    std::span<const uint8_t> data) noexcept {
    TextDatabase db;
    // List descriptor header:
    // [0..1]: descriptor_length (2 bytes)
    // [2]: list_type (0x86)
    // [3]: list_attributes
    // [4..5]: size_of_list_specific_information (2 bytes BE)
    // Followed by list_specific_information, then number_of_entries (2 bytes BE)
    if (data.size() < 8) {
        return db;
    }

    const uint16_t listSpecLen = ReadBE16(&data[4]);
    const size_t entryCountOffset = 6 + listSpecLen;
    if (entryCountOffset + 2 > data.size()) {
        return db;
    }

    const uint16_t numEntries = ReadBE16(&data[entryCountOffset]);
    size_t offset = entryCountOffset + 2;

    uint16_t entryIndex = 0;
    while (offset + 2 <= data.size() && entryIndex < numEntries) {
        const uint16_t entryLen = ReadBE16(&data[offset]);
        offset += 2;
        const size_t entryEnd = std::min(data.size(), offset + entryLen);

        // Entry header:
        // [0]: entry_type (0x93 = Text Database Object)
        // [1]: entry_attributes
        // [2..3]: compound_length / size_of_entry_info
        // [4..6]: 0x00 0x01 0x03
        // Info blocks begin at offset + 7
        if (offset + 7 < entryEnd) {
            const uint8_t entryType = data[offset];
            if (entryType == 0x93 || entryType == 0x90) {
                size_t scan = offset + 7;
                while (scan + 6 <= entryEnd) {
                    const uint16_t blockLen = ReadBE16(&data[scan]);
                    const uint16_t blockType = ReadBE16(&data[scan + 2]);
                    const uint16_t primaryLen = ReadBE16(&data[scan + 4]);

                    if (blockType == 0x000A) { // Text info block
                        const size_t textStart = scan + 6;
                        const size_t textLen = std::min(static_cast<size_t>(primaryLen), entryEnd - textStart);
                        std::string text(reinterpret_cast<const char*>(&data[textStart]), textLen);
                        while (!text.empty() && (text.back() == '\0' || text.back() == '\r' || text.back() == '\n')) {
                            text.pop_back();
                        }
                        db[entryIndex] = std::move(text);
                        break;
                    }

                    if (blockLen == 0 || scan + 2 + blockLen > entryEnd) {
                        break;
                    }
                    scan += 2 + blockLen;
                }
            }
        }

        entryIndex++;
        offset = entryEnd;
    }

    return db;
}

void AudioSubunitDescriptorParser::ResolveNames(
    AudioSubunitIdentifier& identifier, const TextDatabase& textDb) noexcept {
    for (auto& fb : identifier.functionBlocks) {
        if (fb.nameIndex != 0xFFFF) {
            auto it = textDb.find(fb.nameIndex);
            if (it != textDb.end()) {
                fb.name = it->second;
            }
        }
    }
}

} // namespace ASFW::Protocols::AVC::Descriptors
