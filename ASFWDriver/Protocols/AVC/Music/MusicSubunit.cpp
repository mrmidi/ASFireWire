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

void MusicSubunit::ParseCapabilities(AVCUnit& unit, std::function<void(bool)> completion) {
    ASFW_LOG_V1(MusicSubunit, "MusicSubunit: Parsing capabilities...");

    statusDescriptorReadOk_ = false;
    statusDescriptorParsedOk_ = false;
    statusDescriptorHasRouting_ = false;
    statusDescriptorHasClusterInfo_ = false;
    statusDescriptorHasPlugs_ = false;
    statusDescriptorExpectedPlugCount_ = 0;
    musicChannels_.clear();
    plugs_.clear();

    // CRITICAL: Capture shared_ptr to AVCUnit to keep FCPTransport alive during async operations.
    // The DescriptorAccessor stores FCPTransport& as a reference, so the AVCUnit (which owns
    // the FCPTransport via shared_ptr) must stay alive until all callbacks complete.
    // Without this, the FCPTransport reference becomes dangling after OPEN completes but
    // before READ is issued, causing a null pointer crash in FCPTransport::SubmitCommand.
    auto unitPtr = unit.shared_from_this();
    auto accessor = std::make_shared<DescriptorAccessor>(unit, GetAddress());

    // Define specifier for Music Subunit Status Descriptor (0x80)
    // Note: Apple driver uses 0x80 (Status Descriptor) for Music Subunit discovery, not 0x00 (Identifier)
    DescriptorSpecifier specifier;
    specifier.type = static_cast<DescriptorSpecifierType>(0x80); // Status Descriptor
    specifier.typeSpecificFields = {};

    // OPEN -> READ -> CLOSE only (FFADO avc_descriptor.cpp:165-284). A descriptor
    // the device would not open is never read: reading it anyway left firmware
    // descriptor state behind on a Phase 88.
    accessor->readWithOpenCloseSequence(specifier, [this, unitPtr, accessor, completion](const DescriptorAccessor::ReadDescriptorResult& result) {
        if (result.success && !result.data.empty()) {
            ASFW_LOG_V1(MusicSubunit, "MusicSubunit: status descriptor read (%zu bytes)", result.data.size());
            statusDescriptorReadOk_ = true;
            statusDescriptorData_ = result.data; // Store raw data
            ParseDescriptorBlock(result.data.data(), result.data.size());
        } else {
            ASFW_LOG_V0(MusicSubunit, "MusicSubunit: status descriptor unavailable (result=%d)",
                        static_cast<int>(result.avcResult));
        }
        ParseSignalFormats(*unitPtr, completion);
    });
}

void MusicSubunit::ParseSignalFormats(AVCUnit& unit, std::function<void(bool)> completion) {
    // Use comprehensive Stream Format Support command (0xBF) instead of legacy Signal Format (0xA0/0xA1).
    // The legacy commands are often not implemented or are unit-level only.
    ASFW_LOG_V1(MusicSubunit, "MusicSubunit: Querying stream formats...");
    QueryPlugFormats(unit, 0, completion);
}

void MusicSubunit::QueryPlugFormats(AVCUnit& unit, size_t plugIndex, std::function<void(bool)> completion) {
    using namespace StreamFormats;

    // Done with all plugs?
    if (plugIndex >= plugs_.size()) {
        ContinueAfterPlugFormatQueries(unit, completion);
        return;
    }

    auto& plug = plugs_[plugIndex];

    // 0xBF until the unit proves 0x2F-only (Cmd::SendStreamFormat).
    Cmd::StreamFormatCommand cmd{
        .address = SubunitAddress::FromByte(MakeSubunitAddress(GetType(), GetID())),
        .operands = {
            .form = Cmd::StreamFormatSubfunction::kSingle,
            .opcode = Cmd::StreamFormatOpcode::kExtendedStreamFormat,
            .plug = Cmd::PlugAddress::SubunitPlug(plug.IsInput() ? Cmd::PlugDirection::kInput : Cmd::PlugDirection::kOutput, plug.plugID),
        }
    };

    Cmd::SendStreamFormat(unit, cmd, ::ASFW::AVC::CommandType::kStatus,
                          [this, &unit, plugIndex, completion](Expected<Cmd::StreamFormatReply> reply) {
        if (reply && reply->format.rawLength > 0) {
            auto parsed = StreamFormats::StreamFormatParser::Parse(reply->format.rawBytes.data(), reply->format.rawLength);
            HandlePlugFormatResult(plugIndex, AVCResult::kImplementedStable, parsed);
        } else {
            HandlePlugFormatResult(plugIndex, AVCResult::kNotImplemented, std::nullopt);
        }
        QueryPlugFormats(unit, plugIndex + 1, completion);
    });
}

void MusicSubunit::ContinueAfterPlugFormatQueries(AVCUnit& unit, std::function<void(bool)> completion) {
    QuerySupportedFormats(unit, [this, &unit, completion](bool) {
        QueryConnections(unit, [this, &unit, completion](bool) {
            ParsePlugNames(unit, completion);
        });
    });
}

void MusicSubunit::HandlePlugFormatResult(size_t plugIndex,
                                          AVCResult result,
                                          const std::optional<StreamFormats::AudioStreamFormat>& format) {
    using namespace StreamFormats;
    if (!IsSuccess(result) || !format) {
        ASFW_LOG_V3(MusicSubunit, "MusicSubunit: Plug %u format query failed or not implemented",
                    plugs_[plugIndex].plugID);
        return;
    }

    std::vector<ChannelFormatInfo> preservedChannelFormats;
    if (plugs_[plugIndex].currentFormat) {
        preservedChannelFormats = plugs_[plugIndex].currentFormat->channelFormats;
    }

    plugs_[plugIndex].currentFormat = *format;
    if (!preservedChannelFormats.empty()) {
        auto& currentFormats = plugs_[plugIndex].currentFormat->channelFormats;
        for (size_t formatIndex = 0;
             formatIndex < std::min(preservedChannelFormats.size(), currentFormats.size());
             ++formatIndex) {
            currentFormats[formatIndex].channels = std::move(preservedChannelFormats[formatIndex].channels);
        }
        for (size_t formatIndex = currentFormats.size();
             formatIndex < preservedChannelFormats.size();
             ++formatIndex) {
            currentFormats.push_back(std::move(preservedChannelFormats[formatIndex]));
        }
    }

    const uint32_t channelCount = ChannelCountForFormat(*format);
    ASFW_LOG_V1(MusicSubunit, "MusicSubunit: Plug %u (%{public}s) current format: rate=%u Hz, channels=%u",
                plugs_[plugIndex].plugID,
                plugs_[plugIndex].IsInput() ? "in" : "out",
                format->GetSampleRateHz(),
                channelCount);

    const bool hasChannels = std::any_of(musicChannels_.begin(), musicChannels_.end(),
                                         [plugId = plugs_[plugIndex].plugID](const MusicPlugChannel& channel) {
                                             return channel.musicPlugID == plugId;
                                         });
    if (hasChannels) {
        return;
    }

    const size_t totalChannels = format->totalChannels;
    ASFW_LOG_V1(MusicSubunit, "Synthesizing %zu channels for Plug %u",
                totalChannels, plugs_[plugIndex].plugID);

    uint8_t portType = 0x00;
    if (plugs_[plugIndex].type == MusicPlugType::kMIDI) {
        portType = 0x01;
    } else if (plugs_[plugIndex].type == MusicPlugType::kSync) {
        portType = 0x80;
    }

    for (size_t channelIndex = 0; channelIndex < totalChannels; ++channelIndex) {
        MusicPlugChannel channel;
        channel.musicPlugID = plugs_[plugIndex].plugID;
        channel.portType = portType;
        char nameBuf[32];
        snprintf(nameBuf, sizeof(nameBuf), "Channel %zu", channelIndex + 1);
        channel.name = nameBuf;
        musicChannels_.push_back(channel);
    }
}

void MusicSubunit::QuerySupportedFormats(ASFW::Protocols::AVC::IAVCCommandSubmitter& submitter, std::function<void(bool)> completion) {
    using namespace StreamFormats;

    // Helper to recursively query supported formats for each plug
    struct QueryState {
        size_t plugIndex{0};
        std::function<void(bool)> completion;
    };

    auto state = std::make_shared<QueryState>();
    state->completion = completion;

    // Lambda to query next plug
    // Use shared_ptr to allow capturing itself
    auto queryNextPlug = std::make_shared<std::function<void()>>();
    
    *queryNextPlug = [this, &submitter, state, queryNextPlug]() {
        // Done with all plugs?
        if (state->plugIndex >= plugs_.size()) {
            ASFW_LOG_V1(MusicSubunit, "MusicSubunit: Supported format enumeration complete");
            state->completion(true);
            return;
        }

        auto& plug = plugs_[state->plugIndex];
        size_t currentPlugIndex = state->plugIndex;

        ASFW_LOG_V3(MusicSubunit, "MusicSubunit: Querying supported formats for plug %u (%{public}s)",
                      plug.plugID, plug.IsInput() ? "in" : "out");

        auto formats = std::make_shared<std::vector<AudioStreamFormat>>();
        auto iteration = std::make_shared<uint8_t>(0);
        auto queryNextList = std::make_shared<std::function<void()>>();
        auto avcUnit = submitter.AsAvcUnit();
        if (!avcUnit) {
            state->plugIndex++;
            (*queryNextPlug)();
            return;
        }

        *queryNextList = [this, avcUnit, currentPlugIndex, &plug, formats, iteration, state, queryNextPlug, queryNextList]() {
            if (*iteration >= 16) {
                if (!formats->empty()) {
                    plugs_[currentPlugIndex].supportedFormats = std::move(*formats);
                }
                state->plugIndex++;
                (*queryNextPlug)();
                return;
            }

            // 0xBF until the unit proves 0x2F-only (Cmd::SendStreamFormat).
            Cmd::StreamFormatCommand cmd{
                .address = SubunitAddress::FromByte(MakeSubunitAddress(GetType(), GetID())),
                .operands = {
                    .form = Cmd::StreamFormatSubfunction::kList,
                    .opcode = Cmd::StreamFormatOpcode::kExtendedStreamFormat,
                    .plug = Cmd::PlugAddress::SubunitPlug(plug.IsInput() ? Cmd::PlugDirection::kInput : Cmd::PlugDirection::kOutput, plug.plugID),
                    .index = *iteration,
                }
            };

            Cmd::SendStreamFormat(*avcUnit, cmd, ::ASFW::AVC::CommandType::kStatus,
                [formats, iteration, queryNextList, currentPlugIndex, this, state, queryNextPlug](Expected<Cmd::StreamFormatReply> reply) {
                if (reply && reply->format.rawLength > 0) {
                    auto parsed = StreamFormats::StreamFormatParser::Parse(reply->format.rawBytes.data(), reply->format.rawLength);
                    if (parsed) {
                        formats->push_back(*parsed);
                        (*iteration)++;
                        (*queryNextList)();
                        return;
                    }
                }
                if (!formats->empty()) {
                    plugs_[currentPlugIndex].supportedFormats = std::move(*formats);
                }
                state->plugIndex++;
                (*queryNextPlug)();
            });
        };
        (*queryNextList)();
    };

    // Start querying
    (*queryNextPlug)();
}

void MusicSubunit::QueryConnections(ASFW::Protocols::AVC::IAVCCommandSubmitter& submitter, std::function<void(bool)> completion) {
    using namespace StreamFormats;

    auto* avcUnit = submitter.AsAvcUnit();
    if (!avcUnit) {
        ASFW_LOG_V1(MusicSubunit, "MusicSubunit: submitter is not an IAvcUnit");
        completion(false);
        return;
    }

    // Helper to recursively query connections for each destination (input) plug
    struct QueryState {
        size_t plugIndex{0};
        std::function<void(bool)> completion;
        std::function<void()> queryNext; // Recursive function

        void Advance() {
            plugIndex++;
            if (queryNext) {
                queryNext();
            }
        }
    };

    auto state = std::make_shared<QueryState>();
    state->completion = completion;

    // Define the recursive function
    state->queryNext = [this, avcUnit, state]() {
        // Done with all plugs?
        if (state->plugIndex >= plugs_.size()) {
            ASFW_LOG_V1(MusicSubunit, "MusicSubunit: Connection topology query complete");
            auto completion = state->completion;
            state->queryNext = nullptr; // Break reference cycle
            completion(true);
            return;
        }

        auto& plug = plugs_[state->plugIndex];
        size_t currentPlugIndex = state->plugIndex;



        // Only query connection topology for destination (input) plugs
        // Source plugs don't have connections TO them, they have connections FROM them
        if (!plug.IsInput()) {
            state->plugIndex++;
            state->queryNext();
            return;
        }

        ASFW_LOG_V3(MusicSubunit, "MusicSubunit: Querying connection for destination plug %u",
                      plug.plugID);

        // Query SIGNAL SOURCE for this destination plug
        Cmd::SignalSourceCommand cmd{
            .address = SubunitAddress::Unit(),
            .operands = {
                .destination = Cmd::SignalAddress::SubunitPlug(
                    SubunitAddress::FromByte(GetAddress()), plug.plugID),
            },
        };
        const auto expectedDestination = cmd.operands.destination;
        avcUnit->Status(cmd, [this, currentPlugIndex, state, expectedDestination](Expected<Cmd::SignalSource> reply) {
            if (reply && reply->destination == expectedDestination) {
                const auto connInfo = ToConnectionInfo(*reply);
                plugs_[currentPlugIndex].connectionInfo = connInfo;
                LogConnection(currentPlugIndex, connInfo);
            } else if (reply) {
                ASFW_LOG_V3(MusicSubunit,
                            "MusicSubunit: SIGNAL SOURCE destination mismatch for plug %u",
                            plugs_[currentPlugIndex].plugID);
            } else {
                ASFW_LOG_V3(MusicSubunit,
                            "MusicSubunit: SIGNAL SOURCE failed for plug %u (error=%u)",
                            plugs_[currentPlugIndex].plugID,
                            static_cast<unsigned>(reply.error().kind));
            }
            state->Advance();
        });
    };

    // Start querying
    state->queryNext();
}

void MusicSubunit::ParsePlugNames(AVCUnit& unit, std::function<void(bool)> completion) {
    // Plug names are parsed from the descriptor in ParseDescriptorBlock.
    // No additional commands needed if the descriptor was successfully read.

    ASFW_LOG_V1(MusicSubunit, "MusicSubunit: Parsing complete - %zu plugs, "
                "audio=%d midi=%d smpte=%d",
                plugs_.size(),
                capabilities_.hasAudioCapability,
                capabilities_.hasMidiCapability,
                capabilities_.hasSmpteTimeCodeCapability);

    completion(true);
}

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
    parsedStatus_ = std::move(statusOpt);
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
        musicChannels_.push_back(MusicPlugChannel{
            .musicPlugID = mp.musicPlugId,
            .portType = mp.portType,
            .name = mp.name,
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

void MusicSubunit::ReadStatusDescriptor(AVCUnit& unit, std::function<void(bool)> completion) {
    ASFW_LOG_V1(MusicSubunit, "Reading Music Subunit Status Descriptor (type 0x80)");

    auto unitPtr = unit.shared_from_this();
    auto accessor = std::make_shared<DescriptorAccessor>(unit, GetAddress());

    DescriptorSpecifier specifier;
    specifier.type = static_cast<DescriptorSpecifierType>(0x80);
    specifier.typeSpecificFields = {};

    accessor->readWithOpenCloseSequence(specifier, [this, unitPtr, accessor, completion](const DescriptorAccessor::ReadDescriptorResult& result) {
        if (result.success && !result.data.empty()) {
            statusDescriptorReadOk_ = true;
            statusDescriptorData_ = result.data;
            ParseDescriptorBlock(result.data.data(), result.data.size());
            completion(true);
        } else {
            completion(false);
        }
    });
}

void MusicSubunit::SetSampleRate(ASFW::Protocols::AVC::IAVCCommandSubmitter& submitter, uint32_t sampleRate, std::function<void(bool)> completion) {
    using namespace StreamFormats;

    auto avcUnit = submitter.AsAvcUnit();
    if (!avcUnit) {
        ASFW_LOG_V1(MusicSubunit, "MusicSubunit: submitter is not an IAvcUnit");
        completion(false);
        return;
    }

    const auto rateCode = ASFW::AVC::StreamFormatRateFromHz(sampleRate);
    if (!rateCode.has_value()) {
        ASFW_LOG_V1(MusicSubunit, "MusicSubunit: Unsupported sample rate %u Hz", sampleRate);
        completion(false);
        return;
    }

    if (plugs_.empty()) {
        ASFW_LOG_V1(MusicSubunit, "MusicSubunit: No plugs to set sample rate on");
        completion(false);
        return;
    }

    uint8_t plugID = plugs_[0].plugID;
    bool isInput = plugs_[0].IsInput();

    ASFW_LOG_V1(MusicSubunit, "MusicSubunit: Setting sample rate to %u Hz (code 0x%02x) on plug %u", 
                 sampleRate, static_cast<uint8_t>(*rateCode), plugID);

    Cmd::CompoundAm824 compound{
        .rate = *rateCode,
        .syncSource = false,
        .rateControl = Cmd::RateControl::kSupported,
        .entries = { Cmd::CompoundEntry{ .count = 1, .format = Cmd::Am824Format::kMultiBitLinearAudioRaw } },
        .entryCount = 1,
    };

    // 0xBF until the unit proves 0x2F-only (Cmd::SendStreamFormat).
    Cmd::StreamFormatCommand cmd{
        .address = SubunitAddress::FromByte(MakeSubunitAddress(GetType(), GetID())),
        .operands = {
            .form = Cmd::StreamFormatSubfunction::kSingle,
            .opcode = Cmd::StreamFormatOpcode::kExtendedStreamFormat,
            .plug = Cmd::PlugAddress::SubunitPlug(isInput ? Cmd::PlugDirection::kInput : Cmd::PlugDirection::kOutput, plugID),
            .controlFormat = compound,
        }
    };

    Cmd::SendStreamFormat(*avcUnit, cmd, ::ASFW::AVC::CommandType::kControl,
                          [completion](Expected<Cmd::StreamFormatReply> reply) {
        if (reply.has_value()) {
            ASFW_LOG_V1(MusicSubunit, "MusicSubunit: SetSampleRate succeeded");
            completion(true);
        } else {
            ASFW_LOG_V1(MusicSubunit, "MusicSubunit: SetSampleRate failed");
            completion(false);
        }
    });
}

void MusicSubunit::LogConnection(size_t index, const StreamFormats::ConnectionInfo& info) {
    using namespace StreamFormats;
    if (info.sourceSubunitType == SourceSubunitType::kNotConnected) {
        ASFW_LOG_V3(MusicSubunit, "MusicSubunit: Plug %u is not connected",
                      plugs_[index].plugID);
    } else {
        ASFW_LOG_V1(MusicSubunit, "MusicSubunit: Plug %u connected to source plug %u (subunit type 0x%02x, id %u)",
                     plugs_[index].plugID,
                     info.sourcePlugNumber,
                     static_cast<unsigned>(info.sourceSubunitType),
                     info.sourceSubunitID);
    }
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void MusicSubunit::SetAudioVolume(ASFW::Protocols::AVC::IAVCCommandSubmitter& submitter, uint8_t plugId, int16_t volume, std::function<void(bool)> completion) {
    auto* avcUnit = submitter.AsAvcUnit();
    if (!avcUnit) {
        ASFW_LOG_V1(MusicSubunit, "MusicSubunit: submitter is not an IAvcUnit");
        completion(false);
        return;
    }

    Cmd::FeatureCommand cmd{
        .address = SubunitAddress::Of(SubunitType::kAudio, 0),
        .operands = Cmd::FeatureOperands::Volume(
            plugId, Cmd::kMasterChannel, ASFW::AVC::AvcVolume::FromRaw(volume)),
    };
    avcUnit->Control(cmd, [completion = std::move(completion), plugId](Expected<Cmd::FeatureReply> result) mutable {
        if (result) {
            ASFW_LOG_V1(MusicSubunit, "MusicSubunit: Set Audio Volume success (plug %d)", plugId);
        } else {
            ASFW_LOG_V1(MusicSubunit, "MusicSubunit: Set Audio Volume failed: error=%u",
                        static_cast<unsigned>(result.error().kind));
        }
        completion(result.has_value());
    });
}

void MusicSubunit::SetAudioMute(ASFW::Protocols::AVC::IAVCCommandSubmitter& submitter, uint8_t plugId, bool mute, std::function<void(bool)> completion) {
    auto* avcUnit = submitter.AsAvcUnit();
    if (!avcUnit) {
        ASFW_LOG_V1(MusicSubunit, "MusicSubunit: submitter is not an IAvcUnit");
        completion(false);
        return;
    }

    Cmd::FeatureCommand cmd{
        .address = SubunitAddress::Of(SubunitType::kAudio, 0),
        .operands = Cmd::FeatureOperands::Mute(plugId, Cmd::kMasterChannel, mute),
    };
    avcUnit->Control(cmd, [completion = std::move(completion)](Expected<Cmd::FeatureReply> result) mutable {
        if (!result) {
            ASFW_LOG_V1(MusicSubunit, "MusicSubunit: Set Audio Mute failed: error=%u",
                        static_cast<unsigned>(result.error().kind));
        }
        completion(result.has_value());
    });
}

} // namespace ASFW::Protocols::AVC::Music
