//
// MusicSubunit.cpp
// ASFWDriver - AV/C Protocol Layer
//
// Music Subunit implementation (Audio/MIDI interfaces)
//

#include "MusicSubunit.hpp"
#include "../AVCUnit.hpp"
#include "../../../Logging/Logging.hpp"
#include "../Commands/StreamFormatCommand.hpp"
#include "../Commands/StreamFormatDispatch.hpp"
#include "../Core/IAvcUnit.hpp"
#include "../Core/AvcTypes.hpp"
#include "../Commands/SignalSourceCommand.hpp"
#include "../Commands/FunctionBlockCommand.hpp"
#include "../StreamFormats/StreamFormatParser.hpp"
#include "../Descriptors/DescriptorAccessor.hpp"
#include <cctype>
#include <algorithm>
#include <cstdio>
#include <unordered_map>

namespace ASFW::Protocols::AVC::Music {

using ASFW::AVC::IAvcUnit;
using ASFW::AVC::SubunitAddress;
using ASFW::AVC::SubunitType;
using ASFW::AVC::Expected;
using ASFW::AVC::ResponseCode;
namespace Cmd = ASFW::AVC::Cmd;

//==============================================================================
// Helper Functions for Big-Endian Reads
//==============================================================================

namespace {
    inline uint16_t ReadBE16(const uint8_t* data) {
        return (static_cast<uint16_t>(data[0]) << 8) | data[1];
    }
    
    inline uint32_t ReadBE32(const uint8_t* data) {
        return (static_cast<uint32_t>(data[0]) << 24) |
               (static_cast<uint32_t>(data[1]) << 16) |
               (static_cast<uint32_t>(data[2]) << 8) |
                data[3];
    }

    [[nodiscard]] bool BeginCapabilityBlock(const char* blockName,
                                            const uint8_t* specificPtr,
                                            size_t specificAvailableLen,
                                            size_t currentOffset,
                                            uint8_t minLen,
                                            uint8_t& blockLen,
                                            const uint8_t*& blockPtr,
                                            size_t& blockSize) {
        if (specificAvailableLen < currentOffset + 1) {
            return false;
        }

        blockLen = specificPtr[currentOffset];
        blockSize = static_cast<size_t>(blockLen) + 1;
        if (specificAvailableLen < currentOffset + blockSize || blockLen < minLen) {
            ASFW_LOG_V0(MusicSubunit, "%{public}s block invalid (len=%u)", blockName, blockLen);
            return false;
        }

        blockPtr = specificPtr + currentOffset + 1;
        return true;
    }

    [[nodiscard]] StreamFormats::ConnectionInfo ToConnectionInfo(const Cmd::SignalSource& source) noexcept {
        StreamFormats::ConnectionInfo info{};
        if (source.source.bytes[1] == 0xFE) {
            info.sourceSubunitType = StreamFormats::SourceSubunitType::kNotConnected;
            return info;
        }

        const auto address = ASFW::AVC::SubunitAddress::FromByte(source.source.bytes[0]);
        info.sourcePlugNumber = source.source.PlugId();
        if (address.IsUnit()) {
            info.sourceIsExternalUnitPlug = source.source.IsExternalUnitPlug();
            info.sourceSubunitType = StreamFormats::SourceSubunitType::kUnit;
            return info;
        }

        info.sourceSubunitID = address.Id();
        switch (address.Type()) {
            case ASFW::AVC::SubunitType::kAudio:
                info.sourceSubunitType = StreamFormats::SourceSubunitType::kAudio;
                break;
            case ASFW::AVC::SubunitType::kMusic:
                info.sourceSubunitType = StreamFormats::SourceSubunitType::kMusic;
                break;
            default:
                info.sourceSubunitType = StreamFormats::SourceSubunitType::kUnknown;
                break;
        }
        return info;
    }

    [[nodiscard]] bool ParseGeneralCapabilityBlock(MusicSubunitCapabilities& capabilities,
                                                   const uint8_t* specificPtr,
                                                   size_t specificAvailableLen,
                                                   size_t& currentOffset) {
        uint8_t blockLen = 0;
        const uint8_t* blockPtr = nullptr;
        size_t blockSize = 0;
        if (!BeginCapabilityBlock("General Capability", specificPtr, specificAvailableLen, currentOffset, 6,
                                  blockLen, blockPtr, blockSize)) {
            return false;
        }

        capabilities.transmitCapabilityFlags = blockPtr[0];
        capabilities.receiveCapabilityFlags = blockPtr[1];
        capabilities.latencyCapability = ReadBE32(blockPtr + 2);

        ASFW_LOG_V1(MusicSubunit, "General Capability: TxFlags=0x%02x, RxFlags=0x%02x, Latency=%u",
                    capabilities.transmitCapabilityFlags.value(),
                    capabilities.receiveCapabilityFlags.value(),
                    capabilities.latencyCapability.value());

        currentOffset += blockSize;
        return true;
    }

    [[nodiscard]] bool ParseAudioCapabilityBlock(MusicSubunitCapabilities& capabilities,
                                                 const uint8_t* specificPtr,
                                                 size_t specificAvailableLen,
                                                 size_t& currentOffset) {
        uint8_t blockLen = 0;
        const uint8_t* blockPtr = nullptr;
        size_t blockSize = 0;
        if (!BeginCapabilityBlock("Audio Capability", specificPtr, specificAvailableLen, currentOffset, 5,
                                  blockLen, blockPtr, blockSize)) {
            return false;
        }

        const uint8_t numFormats = blockPtr[0];
        const size_t minRequired = 1 + 4 + (static_cast<size_t>(numFormats) * 6);
        if (blockLen < minRequired) {
            ASFW_LOG_V0(MusicSubunit, "Audio Capability data too short for %u formats", numFormats);
            return false;
        }

        capabilities.maxAudioInputChannels = ReadBE16(blockPtr + 1);
        capabilities.maxAudioOutputChannels = ReadBE16(blockPtr + 3);

        std::vector<AudioSampleFormat> formats;
        size_t formatOffset = 5;
        for (uint8_t formatIndex = 0; formatIndex < numFormats; ++formatIndex) {
            if (blockLen < formatOffset + 6) {
                ASFW_LOG_V0(MusicSubunit, "Audio format list truncated at index %u", formatIndex);
                return false;
            }

            AudioSampleFormat format;
            format.raw[0] = blockPtr[formatOffset];
            format.raw[1] = blockPtr[formatOffset + 1];
            format.raw[2] = blockPtr[formatOffset + 2];
            formats.push_back(format);
            formatOffset += 6;
        }
        capabilities.availableAudioFormats = std::move(formats);

        ASFW_LOG_V1(MusicSubunit, "Audio Capability: MaxIn=%u, MaxOut=%u, NumFormats=%u",
                    capabilities.maxAudioInputChannels.value(),
                    capabilities.maxAudioOutputChannels.value(),
                    numFormats);

        currentOffset += blockSize;
        return true;
    }

    [[nodiscard]] bool ParseMidiCapabilityBlock(MusicSubunitCapabilities& capabilities,
                                                const uint8_t* specificPtr,
                                                size_t specificAvailableLen,
                                                size_t& currentOffset) {
        uint8_t blockLen = 0;
        const uint8_t* blockPtr = nullptr;
        size_t blockSize = 0;
        if (!BeginCapabilityBlock("MIDI Capability", specificPtr, specificAvailableLen, currentOffset, 6,
                                  blockLen, blockPtr, blockSize)) {
            return false;
        }

        capabilities.midiVersionMajor = blockPtr[0] >> 4;
        capabilities.midiVersionMinor = blockPtr[0] & 0x0F;
        capabilities.midiAdaptationLayerVersion = blockPtr[1];
        capabilities.maxMidiInputPorts = ReadBE16(blockPtr + 2);
        capabilities.maxMidiOutputPorts = ReadBE16(blockPtr + 4);

        ASFW_LOG_V1(MusicSubunit, "MIDI Capability: Ver=%u.%u, Adapt=0x%02x, MaxIn=%u, MaxOut=%u",
                    capabilities.midiVersionMajor.value(),
                    capabilities.midiVersionMinor.value(),
                    capabilities.midiAdaptationLayerVersion.value(),
                    capabilities.maxMidiInputPorts.value(),
                    capabilities.maxMidiOutputPorts.value());

        currentOffset += blockSize;
        return true;
    }

    [[nodiscard]] bool ParseSingleFlagCapabilityBlock(const char* blockName,
                                                      std::optional<uint8_t>& targetFlags,
                                                      const uint8_t* specificPtr,
                                                      size_t specificAvailableLen,
                                                      size_t& currentOffset) {
        uint8_t blockLen = 0;
        const uint8_t* blockPtr = nullptr;
        size_t blockSize = 0;
        if (!BeginCapabilityBlock(blockName, specificPtr, specificAvailableLen, currentOffset, 1,
                                  blockLen, blockPtr, blockSize)) {
            return false;
        }

        targetFlags = blockPtr[0];
        ASFW_LOG_V1(MusicSubunit, "%{public}s: Flags=0x%02x", blockName, targetFlags.value());
        currentOffset += blockSize;
        return true;
    }

    void ApplyChannelNamesToFormat(StreamFormats::ChannelFormatInfo& channelFormat,
                                   const std::unordered_map<uint16_t, std::string>& channelNameMap,
                                   uint8_t plugId) {
        for (auto& detail : channelFormat.channels) {
            const auto it = channelNameMap.find(detail.musicPlugID);
            if (it == channelNameMap.end()) {
                continue;
            }

            detail.name = it->second;
            ASFW_LOG_V1(MusicSubunit, "Plug %u: Channel 0x%04X -> '%{public}s'",
                        plugId, detail.musicPlugID, detail.name.c_str());
        }
    }
} // namespace

MusicSubunit::MusicSubunit(AVCSubunitType type, uint8_t id)
    : Subunit(type, id) {
    ASFW_LOG_V3(MusicSubunit, "MusicSubunit created: type=0x%02x id=%d",
                   static_cast<uint8_t>(type), id);
}

// ...

bool MusicSubunit::HasCompleteDescriptorParse() const noexcept {
    if (!statusDescriptorReadOk_ || !statusDescriptorParsedOk_) {
        return false;
    }

    if (!statusDescriptorHasRouting_ || !statusDescriptorHasPlugs_) {
        return false;
    }

    if (statusDescriptorExpectedPlugCount_ > 0 &&
        plugs_.size() < statusDescriptorExpectedPlugCount_) {
        return false;
    }

    return true;
}

//==============================================================================
// Music Subunit Identifier Descriptor Parser
// Spec: TA Document 2001007, Section 5.2
//==============================================================================

size_t MusicSubunit::ParseMusicSubunitIdentifier(const uint8_t* data, size_t length) {
    ASFW_LOG_V3(MusicSubunit, "Parsing Music Subunit Identifier Descriptor (%zu bytes)", length);

    // Declare variables at top to avoid goto bypassing initialization errors
    size_t infoBlockOffset = 0;

    // Minimum required: descriptor header + some basic fields
    if (length < 16) {
        ASFW_LOG_V0(MusicSubunit, "Descriptor too short (%zu bytes) for header", length);
        return 0;  // Error - return 0
    }

    // Parse descriptor header
    // uint16_t descriptorLength = ReadBE16(data);  // Usually matches 'length' parameter
    uint8_t generationID = data[2];
    size_t sizeOfListID = data[3];
    size_t sizeOfObjectID = data[4];  // Note: FWA shows this is 1 byte, not 2!
    size_t sizeOfEntryPos = data[5];
    uint16_t numRootLists = ReadBE16(data + 6);
    
    ASFW_LOG_V3(MusicSubunit, "Header: GenID=0x%02x, ListIDSize=%zu, ObjIDSize=%zu, EntryPosSize=%zu, NumRootLists=%u",
                  generationID, sizeOfListID, sizeOfObjectID, sizeOfEntryPos, numRootLists);
    
    // Validate generation ID
    // 0x00: Music Subunit 1.0 (Standard)
    // 0x02: Observed in some devices
    if (generationID != 0x00 && generationID != 0x02) {
        ASFW_LOG_V1(MusicSubunit, "Unexpected generation_ID=0x%02x (expected 0x00 or 0x02)", generationID);
    }
    
    // Calculate offset to subunit_type_dependent_information_length
    size_t rootListArraySize = numRootLists * sizeOfListID;
    size_t subunitDepInfoLenOffset = 8 + rootListArraySize;
    
    if (length < subunitDepInfoLenOffset + 2) {
        ASFW_LOG_V0(MusicSubunit, "Descriptor too short for subunit_type_dependent_information_length at offset %zu (0x%zx)", subunitDepInfoLenOffset, subunitDepInfoLenOffset);
        return 0;  // Error - return 0
    }
    
    uint16_t subunitDepInfoLen = ReadBE16(data + subunitDepInfoLenOffset);
    size_t subunitDepInfoOffset = subunitDepInfoLenOffset + 2;
    
    ASFW_LOG_V3(MusicSubunit, "Subunit dependent info: length=%u, offset=%zu", subunitDepInfoLen, subunitDepInfoOffset);
    
    if (length < subunitDepInfoOffset + subunitDepInfoLen) {
        ASFW_LOG_V0(MusicSubunit, "Descriptor too short for claimed dependent info (len=%u) at offset %zu", subunitDepInfoLen, subunitDepInfoOffset);
        return 0;  // Error - return 0
    }
    
    // Parse Music Subunit specific header within subunit_type_dependent_information
    const uint8_t* musicInfoPtr = data + subunitDepInfoOffset;
    size_t musicInfoAvailableLen = subunitDepInfoLen;
    
    if (musicInfoAvailableLen < 6) {
        ASFW_LOG_V0(MusicSubunit, "Music subunit dependent info too short (%zu bytes)", musicInfoAvailableLen);
        return 0;  // Error - return 0
    }
    
    // Music subunit header: [0-1]=length, [2]=genID, [3]=version, [4-5]=specific_info_length
    capabilities_.musicSubunitVersion = musicInfoPtr[3];
    uint16_t musicSpecificInfoLen = ReadBE16(musicInfoPtr + 4);
    size_t musicSpecificInfoOffset = 6;
    
    ASFW_LOG_V1(MusicSubunit, "Music Subunit Version: 0x%02x, Specific Info Length: %u",
                 capabilities_.musicSubunitVersion, musicSpecificInfoLen);
    
    if (musicInfoAvailableLen < musicSpecificInfoOffset + musicSpecificInfoLen) {
        ASFW_LOG_V0(MusicSubunit, "Music info too short for claimed specific_information length (%u)", musicSpecificInfoLen);
        return 0;  // Error - return 0
    }
    
    // Parse music_subunit_specific_information (capabilities)
    const uint8_t* specificPtr = musicInfoPtr + musicSpecificInfoOffset;
    size_t specificAvailableLen = musicSpecificInfoLen;
    size_t currentOffset = 0;
    
    if (specificAvailableLen < 1) {
        ASFW_LOG_V1(MusicSubunit, "Music specific info area is empty");
        return 0;  // Error - return 0
    }
    
    // Parse capability presence flags (CORRECTED: LSB-first, not MSB-first!)
    uint8_t capAttribs = specificPtr[currentOffset++];
    capabilities_.hasGeneralCapability        = (capAttribs & 0x01) != 0;  // Bit 0
    capabilities_.hasAudioCapability          = (capAttribs & 0x02) != 0;  // Bit 1
    capabilities_.hasMidiCapability           = (capAttribs & 0x04) != 0;  // Bit 2
    capabilities_.hasSmpteTimeCodeCapability  = (capAttribs & 0x08) != 0;  // Bit 3
    capabilities_.hasSampleCountCapability    = (capAttribs & 0x10) != 0;  // Bit 4
    capabilities_.hasAudioSyncCapability      = (capAttribs & 0x20) != 0;  // Bit 5
    
    ASFW_LOG_V3(MusicSubunit, "Capability Flags: 0x%02x [Gen=%d, Aud=%d, MIDI=%d, SMPTE=%d, Samp=%d, Sync=%d]",
                  capAttribs, capabilities_.hasGeneralCapability, capabilities_.hasAudioCapability,
                  capabilities_.hasMidiCapability, capabilities_.hasSmpteTimeCodeCapability,
                  capabilities_.hasSampleCountCapability, capabilities_.hasAudioSyncCapability);
    
    if (capabilities_.hasGeneralCapability &&
        !ParseGeneralCapabilityBlock(capabilities_, specificPtr, specificAvailableLen, currentOffset)) {
        ASFW_LOG_V0(MusicSubunit, "Parse error at offset %zu in music_subunit_specific_information", currentOffset);
        return 0;
    }
    if (capabilities_.hasAudioCapability &&
        !ParseAudioCapabilityBlock(capabilities_, specificPtr, specificAvailableLen, currentOffset)) {
        ASFW_LOG_V0(MusicSubunit, "Parse error at offset %zu in music_subunit_specific_information", currentOffset);
        return 0;
    }
    if (capabilities_.hasMidiCapability &&
        !ParseMidiCapabilityBlock(capabilities_, specificPtr, specificAvailableLen, currentOffset)) {
        ASFW_LOG_V0(MusicSubunit, "Parse error at offset %zu in music_subunit_specific_information", currentOffset);
        return 0;
    }
    if (capabilities_.hasSmpteTimeCodeCapability &&
        !ParseSingleFlagCapabilityBlock("SMPTE Capability",
                                        capabilities_.smpteTimeCodeCapabilityFlags,
                                        specificPtr,
                                        specificAvailableLen,
                                        currentOffset)) {
        ASFW_LOG_V0(MusicSubunit, "Parse error at offset %zu in music_subunit_specific_information", currentOffset);
        return 0;
    }
    if (capabilities_.hasSampleCountCapability &&
        !ParseSingleFlagCapabilityBlock("Sample Count Capability",
                                        capabilities_.sampleCountCapabilityFlags,
                                        specificPtr,
                                        specificAvailableLen,
                                        currentOffset)) {
        ASFW_LOG_V0(MusicSubunit, "Parse error at offset %zu in music_subunit_specific_information", currentOffset);
        return 0;
    }
    if (capabilities_.hasAudioSyncCapability &&
        !ParseSingleFlagCapabilityBlock("Audio SYNC Capability",
                                        capabilities_.audioSyncCapabilityFlags,
                                        specificPtr,
                                        specificAvailableLen,
                                        currentOffset)) {
        ASFW_LOG_V0(MusicSubunit, "Parse error at offset %zu in music_subunit_specific_information", currentOffset);
        return 0;
    }

    // Calculate absolute offset where info blocks start
    // Formula: subunitDepInfoOffset + musicSpecificInfoOffset + currentOffset
    infoBlockOffset = subunitDepInfoOffset + musicSpecificInfoOffset + currentOffset;

    ASFW_LOG_V3(MusicSubunit, "Successfully parsed Music Subunit Identifier Descriptor, info blocks start at offset %zu", infoBlockOffset);
    return infoBlockOffset;
}

void MusicSubunit::ParseDescriptorBlock(const uint8_t* data, size_t length) {
    statusDescriptorParsedOk_ = false;
    statusDescriptorHasRouting_ = false;
    statusDescriptorHasClusterInfo_ = false;
    statusDescriptorHasPlugs_ = false;
    statusDescriptorExpectedPlugCount_ = 0;
    parsedStatus_.reset();
    musicChannels_.clear();
    plugs_.clear();

    if (length < 4) {
        ASFW_LOG_V0(MusicSubunit, "Descriptor too short (%zu bytes)", length);
        return;
    }

    auto statusOpt = Descriptors::MusicSubunitDescriptorParser::ParseStatusDescriptor(
        std::span<const uint8_t>{data, length});
    if (!statusOpt) {
        ASFW_LOG_V0(MusicSubunit, "Failed to parse Music Subunit Status Descriptor (%zu bytes)", length);
        return;
    }

    statusDescriptorReadOk_ = true;
    if (!statusDescriptorData_.has_value()) {
        statusDescriptorData_ = std::vector<uint8_t>(data, data + length);
    }
    parsedStatus_ = std::move(*statusOpt);
    const auto& status = *parsedStatus_;
    ASFW_LOG_V1(MusicSubunit, "Parsed Status Descriptor: Declared Length=%u, Plugs=%zu, MusicPlugs=%zu",
                status.declaredLength, status.plugs.size(), status.musicPlugs.size());

    // 1. General & Media Capabilities
    if (status.capabilities.hasGeneralCapability) {
        capabilities_.hasGeneralCapability = true;
        capabilities_.transmitCapabilityFlags = status.capabilities.transmitCapabilityFlags;
        capabilities_.receiveCapabilityFlags = status.capabilities.receiveCapabilityFlags;
        capabilities_.latencyCapability = status.capabilities.latencyCapability;
    }
    if (status.capabilities.hasAudioCapability) {
        capabilities_.hasAudioCapability = true;
        capabilities_.maxAudioInputChannels = status.capabilities.maxAudioInputChannels;
        capabilities_.maxAudioOutputChannels = status.capabilities.maxAudioOutputChannels;
    }
    if (status.capabilities.hasMidiCapability) {
        capabilities_.hasMidiCapability = true;
        capabilities_.midiVersionMajor = status.capabilities.midiVersionMajor;
        capabilities_.midiVersionMinor = status.capabilities.midiVersionMinor;
        capabilities_.midiAdaptationLayerVersion = status.capabilities.midiAdaptationLayerVersion;
        capabilities_.maxMidiInputPorts = status.capabilities.maxMidiInputPorts;
        capabilities_.maxMidiOutputPorts = status.capabilities.maxMidiOutputPorts;
    }
    if (status.capabilities.hasSmpteTimeCodeCapability) {
        capabilities_.hasSmpteTimeCodeCapability = true;
        capabilities_.smpteTimeCodeCapabilityFlags = status.capabilities.smpteTimeCodeCapabilityFlags;
    }
    if (status.capabilities.hasSampleCountCapability) {
        capabilities_.hasSampleCountCapability = true;
        capabilities_.sampleCountCapabilityFlags = status.capabilities.sampleCountCapabilityFlags;
    }
    if (status.capabilities.hasAudioSyncCapability) {
        capabilities_.hasAudioSyncCapability = true;
        capabilities_.audioSyncCapabilityFlags = status.capabilities.audioSyncCapabilityFlags;
    }

    // 2. Routing Status
    if (status.hasRoutingStatus) {
        statusDescriptorHasRouting_ = true;
        statusDescriptorExpectedPlugCount_ = static_cast<uint16_t>(status.numDestPlugs + status.numSrcPlugs);
    }

    // 3. Music Plug Channels (0x810B)
    for (const auto& mp : status.musicPlugs) {
        std::string name = mp.name;
        if (name.empty()) {
            if (const auto it = status.musicPlugLabels.find(mp.musicPlugId); it != status.musicPlugLabels.end()) {
                name = it->second;
            }
        }
        musicChannels_.push_back(MusicPlugChannel{
            .musicPlugID = mp.musicPlugId,
            .portType = mp.portType,
            .name = std::move(name),
        });
    }

    // 4. Subunit Plugs (0x8109) & Clusters (0x810A)
    for (const auto& p : status.plugs) {
        PlugInfo plug;
        plug.plugID = p.plugId;
        plug.direction = p.isDestination ? StreamFormats::PlugDirection::kInput
                                         : StreamFormats::PlugDirection::kOutput;
        plug.type = (p.usage == 0x04 || p.usage == 0x05)
            ? StreamFormats::MusicPlugType::kAudio
            : static_cast<StreamFormats::MusicPlugType>(p.usage);
        plug.name = p.name;

        if (!p.clusters.empty()) {
            statusDescriptorHasClusterInfo_ = true;
            StreamFormats::AudioStreamFormat currentFormat{};
            for (const auto& cluster : p.clusters) {
                StreamFormats::ChannelFormatInfo channelFormat;
                channelFormat.formatCode = static_cast<StreamFormats::StreamFormatCode>(cluster.streamFormatCode);
                channelFormat.channelCount = cluster.channelCount;
                for (const auto& sig : cluster.signals) {
                    StreamFormats::ChannelFormatInfo::ChannelDetail detail;
                    detail.musicPlugID = sig.musicPlugId;
                    detail.position = sig.position;
                    channelFormat.channels.push_back(detail);
                }
                currentFormat.channelFormats.push_back(std::move(channelFormat));
            }
            plug.currentFormat = std::move(currentFormat);
        }
        plugs_.push_back(std::move(plug));
    }

    // 5. Finalize Plugs: apply channel names from musicChannels_ and update capabilities
    if (!plugs_.empty()) {
        statusDescriptorHasPlugs_ = true;
        ApplyMusicChannelNamesToPlugs();
        UpdateCapabilitiesFromPlugs();
    }

    statusDescriptorParsedOk_ = statusDescriptorHasPlugs_ || statusDescriptorHasRouting_;
}

void MusicSubunit::ApplyMusicChannelNamesToPlugs() {
    std::unordered_map<uint16_t, std::string> channelNameMap;
    for (const auto& channel : musicChannels_) {
        if (!channel.name.empty()) {
            channelNameMap[channel.musicPlugID] = channel.name;
        }
    }

    for (auto& plug : plugs_) {
        if (!plug.currentFormat) {
            continue;
        }

        for (auto& channelFormat : plug.currentFormat->channelFormats) {
            ApplyChannelNamesToFormat(channelFormat, channelNameMap, plug.plugID);
        }
    }
}

uint16_t MusicSubunit::ChannelCountForFormat(const StreamFormats::AudioStreamFormat& format) noexcept {
    if (format.totalChannels > 0) {
        return format.totalChannels;
    }

    uint32_t sum = 0;
    for (const auto& block : format.channelFormats) {
        sum += block.channelCount;
    }
    return (sum > 0) ? static_cast<uint16_t>(std::min<uint32_t>(sum, 0xFFFFu)) : 0;
}

void MusicSubunit::UpdateCapabilitiesFromPlugs() {
    if (plugs_.empty()) {
        return;
    }

    for (const auto& plug : plugs_) {
        if (plug.type == ASFW::Protocols::AVC::StreamFormats::MusicPlugType::kAudio) {
            capabilities_.hasAudioCapability = true;
        } else if (plug.type == ASFW::Protocols::AVC::StreamFormats::MusicPlugType::kMIDI) {
            capabilities_.hasMidiCapability = true;
        }
    }

    uint16_t audioInputPlugs = 0;
    uint16_t audioOutputPlugs = 0;
    uint16_t audioInputMaxChannels = capabilities_.maxAudioInputChannels.value_or(0);
    uint16_t audioOutputMaxChannels = capabilities_.maxAudioOutputChannels.value_or(0);
    uint16_t midiIns = 0;
    uint16_t midiOuts = 0;

    for (const auto& plug : plugs_) {
        if (plug.type == ASFW::Protocols::AVC::StreamFormats::MusicPlugType::kAudio) {
            const uint16_t channels = plug.currentFormat ? ChannelCountForFormat(*plug.currentFormat) : 0;
            if (plug.IsInput()) {
                ++audioInputPlugs;
                audioInputMaxChannels = std::max(audioInputMaxChannels, channels);
            } else {
                ++audioOutputPlugs;
                audioOutputMaxChannels = std::max(audioOutputMaxChannels, channels);
            }
        } else if (plug.type == ASFW::Protocols::AVC::StreamFormats::MusicPlugType::kMIDI) {
            if (plug.IsInput()) {
                ++midiIns;
            } else {
                ++midiOuts;
            }
        }
    }

    if (audioInputMaxChannels > 0) {
        capabilities_.maxAudioInputChannels = audioInputMaxChannels;
    }
    if (audioOutputMaxChannels > 0) {
        capabilities_.maxAudioOutputChannels = audioOutputMaxChannels;
    }
    capabilities_.maxMidiInputPorts = midiIns;
    capabilities_.maxMidiOutputPorts = midiOuts;

    ASFW_LOG_V1(MusicSubunit,
                "Updated Capabilities from Plugs: Audio In maxCh=%u (plugs=%u) Out maxCh=%u (plugs=%u), MIDI In=%u Out=%u",
                capabilities_.maxAudioInputChannels.value_or(0), audioInputPlugs,
                capabilities_.maxAudioOutputChannels.value_or(0), audioOutputPlugs,
                midiIns, midiOuts);
}

} // namespace ASFW::Protocols::AVC::Music

void ASFW::Protocols::AVC::Music::MusicSubunit::LoadSnapshot(const ASFW::AVC::DiscoveryEngine::DiscoverySnapshot& snapshot) {
    const ASFW::AVC::SubunitId id{ASFW::AVC::SubunitType::kMusic, GetID()};
    for (const auto& blob : snapshot.descriptors) if (blob.subunit == id && !blob.primaryError && !blob.bytes.empty()) {
        statusDescriptorData_ = blob.bytes;
        ParseDescriptorBlock(blob.bytes.data(), blob.bytes.size());
    }
    for (const auto& fact : snapshot.plugs) {
        if (fact.address != id.ToAddress()) continue;
        auto found = std::find_if(plugs_.begin(), plugs_.end(), [&](const auto& plug) {
            return plug.plugID == fact.id.value && plug.IsInput() == (fact.direction == ASFW::AVC::Cmd::PlugDirection::kInput);
        });
        if (found == plugs_.end()) continue;
        if (fact.current) {
            auto raw = fact.current->Raw();
            if (auto format = StreamFormats::StreamFormatParser::Parse(raw.data(), raw.size())) found->currentFormat = *format;
        }
        for (const auto& formation : fact.formations) {
            auto raw = formation.Raw();
            if (auto format = StreamFormats::StreamFormatParser::Parse(raw.data(), raw.size())) found->supportedFormats.push_back(*format);
        }
        if (fact.route) {
            const auto& source = fact.route->source;
            found->connectionInfo = StreamFormats::ConnectionInfo{
                .sourceSubunitType = static_cast<StreamFormats::SourceSubunitType>(source.Subunit().Type()),
                .sourceSubunitID = source.Subunit().Id(), .sourcePlugNumber = source.PlugId(),
                .sourceIsExternalUnitPlug = source.IsExternalUnitPlug()};
        }
    }
    UpdateCapabilitiesFromPlugs();
}
